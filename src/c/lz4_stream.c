#include "lz4_stream.h"

#include <string.h>
#include <limits.h>
#include <assert.h>

#define PHASE_READ_TOK			0
#define PHASE_READ_EX_LIT_LEN	1
#define PHASE_COPY_LIT			2
#define PHASE_READ_OFS			3
#define PHASE_READ_OFS2			4
#define PHASE_READ_EX_MAT_LEN	5
#define PHASE_COPY_MAT			6

#define PHASE_REPORT_ERROR		7

#define MAX_BLOCK_LEN			UINT_MAX

#define O_BUF_LEN 				0x10000
#define O_BUF_PAD				32 //allows sloppy reads/writes at start+end

_Static_assert(sizeof(((lz4_dec_stream_state *)0)->p_.o_buf) == O_BUF_PAD + O_BUF_LEN + O_BUF_PAD, "fix O_BUF_LEN + O_BUF_PAD");
_Static_assert((O_BUF_LEN & (O_BUF_LEN - 1)) == 0, "o_buf not pow2 size; fix below");
#define WRAP_OBUF_IDX(idx) 		((idx) & (O_BUF_LEN - 1))

/*
	Helper macros to make the state machine easier to see.
*/

#if defined(_MSC_VER)
	#define ASSUME(fact)				__assume(fact)
	#define LIKELY(x)					(x)
	#define UNLIKELY(x)					(x)
	#define STREAM_RUN_UNREACHABLE()	__assume(0)
#elif defined(__GNUC__)
	#ifdef __clang__
		#define ASSUME(fact)			__builtin_assume(fact)
	#else
		#define ASSUME(fact)			__attribute((assume(fact)))
	#endif
	#define LIKELY(x)					__builtin_expect(!!(x), 1)
	#define UNLIKELY(x)					__builtin_expect(!!(x), 0)
	#define STREAM_RUN_UNREACHABLE()	__builtin_unreachable()
#else
	#define STREAM_RUN_UNREACHABLE()	goto phase_REPORT_ERROR
#endif

#if defined(__clang__)
	#define MACRO_IF_BLOCK_(cond, ...) \
		_Pragma("GCC diagnostic push") \
		_Pragma("GCC diagnostic ignored \"-Wdangling-else\"") \
		if (cond) __VA_ARGS__ else ((void)0) \
		_Pragma("GCC diagnostic pop")
#else
	#if defined(__GNUC__)
	#pragma GCC diagnostic ignored "-Wdangling-else" //GCC doesn't like the way we _Pragma
	#endif

	#define MACRO_IF_BLOCK_(cond, ...) \
		if (cond) __VA_ARGS__ else ((void)0)
#endif

#define LITTLE_ENDIAN	1
#define BIG_ENDIAN		2

#ifndef LZ4_BYTE_ORDER
	#if (defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN) || \
		defined(__LITTLE_ENDIAN__) || \
		defined(__ARMEL__) || \
		defined(__THUMBEL__) || \
		defined(__AARCH64EL__) || \
		defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__) || \
		(defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64) || defined(_M_IA64) || defined(_M_ARM)))

		#define LZ4_BYTE_ORDER LITTLE_ENDIAN

	#elif (defined(__BYTE_ORDER) && __BYTE_ORDER == __BIG_ENDIAN) || \
		defined(__BIG_ENDIAN__) || \
		defined(__ARMEB__) || \
		defined(__THUMBEB__) || \
		defined(__AARCH64EB__) || \
		defined(_MIBSEB) || defined(__MIBSEB) || defined(__MIBSEB__) || \
		(defined(_MSC_VER) && (defined(_M_PPC)))

		#define LZ4_BYTE_ORDER BIG_ENDIAN

	#else
		#error "Unable to determine target byte order"
	#endif
#endif

#if LZ4_BYTE_ORDER == LITTLE_ENDIAN
	#define RBOS >>		// right-shift on little endian; left-shift on BE
	#define LBOS <<		// left-shift on little endian; right-shift on BE
#elif LZ4_BYTE_ORDER == BIG_ENDIAN
	#define RBOS <<		// left-shift on big endian; right-shift on LE
	#define LBOS >>		// right-shift o nbig endian; left-shift on LE
#else
	#error "No fallback available for unknown endianness."
#endif

#if CHAR_BIT != 8
	#error "This code is probably all kinds of incompatible with odd byte lengths."
#endif

// mask off all but the N right-most (little-endian; leftmost on BE) bytes
#define MASK_N(type, n) ((type)-1 RBOS (sizeof(uintptr_t) - (n)) * 8)

#define STREAM_RUN_PROLOG() \
	/* pull s apart into stack locals */ \
	\
	const uint8_t* restrict in = s->in; \
	const uint8_t* restrict const in_end = s->in + s->avail_in; \
	\
	uint8_t* restrict out = s->out; \
	size_t avail_out = s->avail_out; \
	\
	uint8_t* restrict const o_buf = s->p_.o_buf + O_BUF_PAD; \
	unsigned int o_pos = s->p_.o_pos; \
	\
	unsigned int lit_len = s->p_.lit_len; /* the length of the current literal */ \
	unsigned int mat_len = s->p_.mat_len; /* the length of the current match */ \
	unsigned int mat_dst = s->p_.mat_dst; /* the distance to the current match */ \
	\
	unsigned int phase = s->p_.phase

#define STREAM_RESUME_FROM_SUSPEND() \
	switch (phase) \
	{ \
	case PHASE_READ_TOK: 		goto phase_READ_TOK; \
	case PHASE_READ_EX_LIT_LEN:	goto phase_READ_EX_LIT_LEN; \
	case PHASE_COPY_LIT:		goto phase_COPY_LIT; \
	case PHASE_READ_OFS:		goto phase_READ_OFS; \
	case PHASE_READ_OFS2:		goto phase_READ_OFS2; \
	case PHASE_READ_EX_MAT_LEN:	goto phase_READ_EX_MAT_LEN; \
	case PHASE_COPY_MAT:		goto phase_COPY_MAT; \
	case PHASE_REPORT_ERROR: 	goto phase_REPORT_ERROR; \
	default: \
		assert(0 && "corrupt decoder stream state"); \
		/* this is a programmer error, not bad input */ \
		STREAM_RUN_UNREACHABLE(); \
	}

#define TRANSITION_TO_PHASE(next_phase) \
	MACRO_IF_BLOCK_(1, {phase = PHASE_##next_phase; goto phase_##next_phase;})
#define SUSPEND_FOR_NOW() \
	goto suspend_for_now
#define SUSPEND_IF_INPUT_EMPTY() \
	MACRO_IF_BLOCK_(in == in_end, SUSPEND_FOR_NOW();)

#define STREAM_RUN_SUSPEND_EPILOG() \
	s->in = in; \
	s->avail_in = in_end - in; \
	\
	s->out = out; \
	s->avail_out = avail_out; \
	\
	s->p_.lit_len = lit_len; \
	s->p_.mat_len = mat_len; \
	s->p_.o_pos = o_pos; \
	s->p_.mat_dst = mat_dst; \
	\
	s->p_.phase = phase

void lz4_dec_stream_init(lz4_dec_stream_state *s)
{
	s->in = 0;
	s->avail_in = 0;
	
	s->out = 0;
	s->avail_out = 0;

	s->p_.o_pos = 0;

	s->p_.lit_len = 0;
	s->p_.mat_len = 0;
	s->p_.mat_dst = 0;

	s->p_.phase = PHASE_READ_TOK;
}

int lz4_dec_stream_run(lz4_dec_stream_state *s)
{
	STREAM_RUN_PROLOG();

	uint8_t *out_start = s->out;

	STREAM_RESUME_FROM_SUSPEND();

phase_READ_TOK: //read a token
	{
		SUSPEND_IF_INPUT_EMPTY();
		uint8_t c = *in++;

		lit_len = c >> 4;
		mat_len = (c & 0xF) + 4;
	}

	switch (lit_len)
	{
	case 0: TRANSITION_TO_PHASE(READ_OFS); //we just read a match
	case 0xF: TRANSITION_TO_PHASE(READ_EX_LIT_LEN); //we have a long literal, read more length bytes
	default: TRANSITION_TO_PHASE(COPY_LIT); //copy lit_len bytes to the output
	}

phase_READ_EX_LIT_LEN: //loop; read an additional byte of literal length
	{
		SUSPEND_IF_INPUT_EMPTY();
		uint8_t c = *in++;

		if (c > MAX_BLOCK_LEN - lit_len)
			TRANSITION_TO_PHASE(REPORT_ERROR);

		lit_len += c;

		if (c == 0xFF)
			goto phase_READ_EX_LIT_LEN; //loop
	}

	TRANSITION_TO_PHASE(COPY_LIT);

phase_COPY_LIT: //copy lit_len bytes from the input to the output
	assert(lit_len > 0);
	{
		unsigned int clamped_lit_len = lit_len;

		size_t avail_in = in_end - in;
		if (clamped_lit_len > avail_in)
			clamped_lit_len = (unsigned int)avail_in;
		if (clamped_lit_len > avail_out)
			clamped_lit_len = (unsigned int)avail_out;

		memcpy(out, in, clamped_lit_len);
		in += clamped_lit_len;
		out += clamped_lit_len;

		avail_out -= clamped_lit_len;
		lit_len -= clamped_lit_len;
	}

	if (lit_len)
		//there's more literal to copy, but either src or dst bufs ran out
		SUSPEND_FOR_NOW();

	TRANSITION_TO_PHASE(READ_OFS);

phase_READ_OFS: //read the first byte of a match offset
	SUSPEND_IF_INPUT_EMPTY();
	mat_dst = *in++;

	TRANSITION_TO_PHASE(READ_OFS2);

phase_READ_OFS2: //read the second byte of a match offset
	SUSPEND_IF_INPUT_EMPTY();
	mat_dst |= (unsigned int)*in++ << 8;
	_Static_assert(0xFFFF < O_BUF_LEN, "mat_dst must never reach beyond o_buf");

	if (!mat_dst)
		TRANSITION_TO_PHASE(REPORT_ERROR);

	if (mat_len == 0xF + 4)
		TRANSITION_TO_PHASE(READ_EX_MAT_LEN);
	else
		TRANSITION_TO_PHASE(COPY_MAT);

phase_READ_EX_MAT_LEN: //loop; read an additional byte of match length
	{
		SUSPEND_IF_INPUT_EMPTY();
		uint8_t c = *in++;

		if (c > MAX_BLOCK_LEN - mat_len)
			TRANSITION_TO_PHASE(REPORT_ERROR);

		mat_len += c;

		if (c == 0xFF)
			goto phase_READ_EX_MAT_LEN; //loop
	}

	TRANSITION_TO_PHASE(COPY_MAT);

phase_COPY_MAT: //copy mat_len bytes from mat_dst bytes behind the output cursor
	assert(mat_len > 0);
	{
		//nb: mat_dst will not be more than O_BUF_LEN
		unsigned int clamped_mat_len = mat_len < avail_out ?
			mat_len :
			(unsigned int)avail_out;

		if (clamped_mat_len)
		{
			size_t n_in_out = out - out_start;
			if (mat_dst > n_in_out)
			{
				//we're reading far enough back that we need to hit the buffer

				//figure out how far back into the buffer we need to go
				unsigned int buf_dst = mat_dst - (unsigned int)n_in_out; //nb: n_in_out <= mat_dst
				//and how many bytes we'll pull from it
				unsigned int buf_cnt = buf_dst < clamped_mat_len ? buf_dst : clamped_mat_len;

				//and exactly where in the buffer we'll copy from
				unsigned int buf_src = WRAP_OBUF_IDX(o_pos - buf_dst);

				unsigned int e = buf_src + buf_cnt;
				if (e > O_BUF_LEN)
				{
					e = O_BUF_LEN - buf_src;
					memcpy(out, o_buf + buf_src, e);
					memcpy(out + e, o_buf, buf_cnt - e);
				}
				else
				{
					memcpy(out, o_buf + buf_src, buf_cnt);
				}

				out += buf_cnt;
				avail_out -= buf_cnt;

				clamped_mat_len -= buf_cnt;
				mat_len -= buf_cnt;
			}

			size_t c = clamped_mat_len;
			const uint8_t *out_src = out - mat_dst;
			while (c--)
				*out++ = *out_src++;

			avail_out -= clamped_mat_len;
			mat_len -= clamped_mat_len;
		}
	}

	if (mat_len)
		//we ran out of avail_out before we finished
		SUSPEND_FOR_NOW();

	TRANSITION_TO_PHASE(READ_TOK);

suspend_for_now:
	//tuck everything away for the next call
	{
		size_t len = out - out_start;
		if (len >= O_BUF_LEN)
		{
			memcpy(o_buf, out - O_BUF_LEN, O_BUF_LEN);
			o_pos = 0;
		}
		else //nb: len < O_BUF_LEN
		{
			unsigned int e = o_pos + (unsigned int)len;
			if (e > O_BUF_LEN)
			{
				e = O_BUF_LEN - o_pos;
				memcpy(o_buf + o_pos, out - len, e);

				o_pos = (unsigned int)len - e;
				memcpy(o_buf, out - o_pos, o_pos);
			}
			else
			{
				memcpy(o_buf + o_pos, out - len, len);
				o_pos += (unsigned int)len;
				if (o_pos == O_BUF_LEN) o_pos = 0;
			}
		}
	}

	STREAM_RUN_SUSPEND_EPILOG();
	return 0;

phase_REPORT_ERROR:
	s->p_.phase = PHASE_REPORT_ERROR;
	return -1;
}

static unsigned int lz4_dec_cpy_mat_no_overlap(
	unsigned int copy_mat_len,
	unsigned int o_inpos, unsigned int o_pos,
	uint8_t* restrict o_buf, uint8_t* restrict out)
{
	for (unsigned int copy_len, copy_mat_len_left = copy_mat_len; copy_mat_len_left != 0; copy_mat_len_left -= copy_len)
	{
		copy_len = copy_mat_len_left;

		unsigned int pos_avail = O_BUF_LEN - o_pos;
		if (UNLIKELY(copy_len > pos_avail))
			copy_len = pos_avail;

		unsigned int inpos_avail = O_BUF_LEN - o_inpos;
		if (UNLIKELY(copy_len > inpos_avail))
			copy_len = inpos_avail;

		memcpy(o_buf + o_pos, o_buf + o_inpos, copy_len);
		memcpy(out, o_buf + o_inpos, copy_len);

		o_pos = WRAP_OBUF_IDX(o_pos + copy_len);
		o_inpos = WRAP_OBUF_IDX(o_inpos + copy_len);

		out += copy_len;
	}

	return copy_mat_len;
}

static unsigned int lz4_dec_cpy_mat_rle_long_dst(
	unsigned int copy_mat_len,
	unsigned int o_inpos, unsigned int o_pos,
	uint8_t* restrict o_buf, uint8_t* restrict out)
{
	_Static_assert(sizeof(uintptr_t) <= O_BUF_PAD, "padding insufficient for sloppy reads");

	unsigned int n_copied = 0;

	while (copy_mat_len >= sizeof(uintptr_t))
	{
		uintptr_t c;

		//read the next word from o_buf's read cursor

		memcpy(&c, o_buf + o_inpos, sizeof(c));

		unsigned int n_read = O_BUF_LEN - o_inpos;
		if (UNLIKELY(n_read < sizeof(c)))
		{
			//we read off the end of o_buf's active area into the scratch space
			//read from the beginning and patch the read values

			uintptr_t c2;
			memcpy(&c2, o_buf, sizeof(c2));

			c &= MASK_N(uintptr_t, n_read);
			c |= c2 LBOS n_read * 8;
		}
		o_inpos = WRAP_OBUF_IDX(o_inpos + sizeof(c));

		//write the word back to o_buf's write cursor

		memcpy(o_buf + o_pos, &c, sizeof(c));

		unsigned int n_written = O_BUF_LEN - o_pos;
		o_pos = WRAP_OBUF_IDX(o_pos + sizeof(c));

		if (UNLIKELY(n_written < sizeof(c)))
			//some bytes went into the scratch pad past the end
			//need to copy those to the beginning of the buffer
			memcpy(o_buf + o_pos - sizeof(c), &c, sizeof(c));

		memcpy(out + n_copied, &c, sizeof(c));
		n_copied += sizeof(c);

		copy_mat_len -= sizeof(c);
	}

	return n_copied;
}

static unsigned int lz4_dec_cpy_mat_rle_short_dst(
	unsigned int copy_mat_len, unsigned int mat_dst,
	unsigned int o_inpos, unsigned int o_pos,
	uint8_t* restrict o_buf, uint8_t* restrict out)
{
	_Static_assert(sizeof(uintptr_t) <= O_BUF_PAD, "padding insufficient for sloppy reads");
	assert(mat_dst < sizeof(uintptr_t));
	ASSUME(mat_dst < sizeof(uintptr_t));

	if (copy_mat_len < sizeof(uintptr_t))
		return 0;

	unsigned int n_copied = 0;

	uintptr_t c;
	memcpy(&c, o_buf + o_inpos, sizeof(c));
	unsigned int n_read = O_BUF_LEN - o_inpos;
	if (UNLIKELY(n_read < mat_dst))
	{
		uintptr_t c2;
		memcpy(&c2, o_buf, sizeof(c2));

		c &= MASK_N(uintptr_t, n_read);
		c |= c2 LBOS n_read * 8;
	}

	c &= MASK_N(uintptr_t, mat_dst);
	for (unsigned int n = mat_dst; n < sizeof(uintptr_t); n *= 2)
		c |= c LBOS n * 8;

	unsigned int shift = (sizeof(uintptr_t) % mat_dst) * 8;

	while (copy_mat_len >= sizeof(c))
	{
		memcpy(o_buf + o_pos, &c, sizeof(c));

		unsigned int n_written = O_BUF_LEN - o_pos;
		o_pos = WRAP_OBUF_IDX(o_pos + sizeof(c));

		if (UNLIKELY(n_written < sizeof(c)))
			//some bytes went into the scratch pad past the end
			//need to copy those to the beginning of the buffer
			memcpy(o_buf + o_pos - sizeof(c), &c, sizeof(c));

		memcpy(out + n_copied, &c, sizeof(c));
		n_copied += sizeof(c);

		c = c RBOS shift;
		c |= c LBOS (sizeof(uintptr_t) * 8 - shift);

		copy_mat_len -= sizeof(c);
	}

	return n_copied;
}

static void lz4_dec_cpy_mat_bytes(
	unsigned int copy_mat_len, unsigned int o_inpos, unsigned int o_pos,
	uint8_t* restrict o_buf, uint8_t* restrict out)
{
	for (unsigned int i = 0; i < copy_mat_len; i++)
	{
		uint8_t c = o_buf[o_inpos];
		o_inpos = WRAP_OBUF_IDX(o_inpos + 1);

		o_buf[o_pos] = c;
		o_pos = WRAP_OBUF_IDX(o_pos + 1);

		*out++ = c;
	}
}

int lz4_dec_stream_run_dst_uncached(lz4_dec_stream_state* s)
{
	STREAM_RUN_PROLOG();
	STREAM_RESUME_FROM_SUSPEND();

phase_READ_TOK: //read a token
	{
		SUSPEND_IF_INPUT_EMPTY();
		uint8_t c = *in++;

		lit_len = c >> 4;
		mat_len = (c & 0xF) + 4;
	}

	switch (lit_len)
	{
	case 0: TRANSITION_TO_PHASE(READ_OFS); //we just read a match
	case 0xF: TRANSITION_TO_PHASE(READ_EX_LIT_LEN); //we have a long literal, read more length bytes
	default: TRANSITION_TO_PHASE(COPY_LIT); //copy lit_len bytes to the output
	}

phase_READ_EX_LIT_LEN: //loop; read an additional byte of literal length
	{
		SUSPEND_IF_INPUT_EMPTY();
		uint8_t c = *in++;

		if (c > MAX_BLOCK_LEN - lit_len)
			TRANSITION_TO_PHASE(REPORT_ERROR);

		lit_len += c;

		if (c == 0xFF)
			goto phase_READ_EX_LIT_LEN; //loop
	}

	TRANSITION_TO_PHASE(COPY_LIT);

phase_COPY_LIT: //copy lit_len bytes from the input to the output
	assert(lit_len > 0);
	{
		unsigned int clamped_lit_len = lit_len;

		size_t avail_in = in_end - in;
		if (clamped_lit_len > avail_in)
			clamped_lit_len = (unsigned int)avail_in;
		if (clamped_lit_len > avail_out)
			clamped_lit_len = (unsigned int)avail_out;

		if (clamped_lit_len > O_BUF_LEN)
		{
			unsigned int first_copy_len = clamped_lit_len - O_BUF_LEN;
			memcpy(out, in, first_copy_len);
			in += first_copy_len;
			out += first_copy_len;

			memcpy(o_buf, in, O_BUF_LEN);
			in += O_BUF_LEN;
			memcpy(out, o_buf, O_BUF_LEN);
			out += O_BUF_LEN;

			o_pos = 0;
		}
		else
		{
			unsigned int o_buf_avail = O_BUF_LEN - o_pos;

			unsigned int first_copy_len = clamped_lit_len < o_buf_avail ? clamped_lit_len : o_buf_avail;
			memcpy(o_buf + o_pos, in, first_copy_len);
			in += first_copy_len;
			memcpy(out, o_buf + o_pos, first_copy_len);
			out += first_copy_len;

			o_pos = WRAP_OBUF_IDX(o_pos + first_copy_len);

			unsigned int second_copy_len = clamped_lit_len - first_copy_len;
			if (second_copy_len)
			{
				memcpy(o_buf, in, second_copy_len);
				in += second_copy_len;
				memcpy(out, o_buf, second_copy_len);
				out += second_copy_len;

				o_pos = second_copy_len;
			}
		}

		avail_out -= clamped_lit_len;
		lit_len -= clamped_lit_len;
	}

	if (lit_len)
		//there's more literal to copy, but either src or dst bufs ran out
		SUSPEND_FOR_NOW();

	TRANSITION_TO_PHASE(READ_OFS);

phase_READ_OFS: //read the first byte of a match offset
	SUSPEND_IF_INPUT_EMPTY();
	mat_dst = *in++;

	TRANSITION_TO_PHASE(READ_OFS2);

phase_READ_OFS2: //read the second byte of a match offset
	SUSPEND_IF_INPUT_EMPTY();
	mat_dst |= (unsigned int)*in++ << 8;
	_Static_assert(0xFFFF < O_BUF_LEN, "mat_dst must never reach beyond o_buf");

	if (!mat_dst)
		TRANSITION_TO_PHASE(REPORT_ERROR);

	if (mat_len == 0xF + 4)
		TRANSITION_TO_PHASE(READ_EX_MAT_LEN);
	else
		TRANSITION_TO_PHASE(COPY_MAT);

phase_READ_EX_MAT_LEN: //loop; read an additional byte of match length
	{
		SUSPEND_IF_INPUT_EMPTY();
		uint8_t c = *in++;

		if (c > MAX_BLOCK_LEN - mat_len)
			TRANSITION_TO_PHASE(REPORT_ERROR);

		mat_len += c;

		if (c == 0xFF)
			goto phase_READ_EX_MAT_LEN; //loop
	}

	TRANSITION_TO_PHASE(COPY_MAT);

phase_COPY_MAT: //copy mat_len bytes from mat_dst bytes behind the output cursor
	assert(mat_len > 0);
	{
		//nb: mat_dst will not be more than O_BUF_LEN
		unsigned int o_inpos = WRAP_OBUF_IDX(o_pos - mat_dst);

		unsigned int clamped_mat_len = mat_len;
		if (clamped_mat_len > avail_out)
			clamped_mat_len = (unsigned int)avail_out;

		unsigned int copy_mat_len = clamped_mat_len;

		_Static_assert(sizeof(uintptr_t) < 16, "fix below");
		unsigned int n_copied;
		if (mat_dst >= copy_mat_len)
			n_copied = lz4_dec_cpy_mat_no_overlap(copy_mat_len, o_inpos, o_pos, o_buf, out);
		else if (mat_dst >= sizeof(uintptr_t))
			n_copied = lz4_dec_cpy_mat_rle_long_dst(copy_mat_len, o_inpos, o_pos, o_buf, out);
		else
			n_copied = lz4_dec_cpy_mat_rle_short_dst(copy_mat_len, mat_dst, o_inpos, o_pos, o_buf, out);

		o_inpos = WRAP_OBUF_IDX(o_inpos + n_copied);
		o_pos = WRAP_OBUF_IDX(o_pos + n_copied);
		copy_mat_len -= n_copied;
		out += n_copied;

		//consume any remaining bytes past the end of the last block
		lz4_dec_cpy_mat_bytes(copy_mat_len, o_inpos, o_pos, o_buf, out);
		o_pos = WRAP_OBUF_IDX(o_pos + copy_mat_len);
		out += copy_mat_len;

		avail_out -= clamped_mat_len;
		mat_len -= clamped_mat_len;
	}

	if (mat_len)
		//we ran out of avail_out before we finished
		SUSPEND_FOR_NOW();

	TRANSITION_TO_PHASE(READ_TOK);

suspend_for_now:
	//tuck everything away for the next call

	STREAM_RUN_SUSPEND_EPILOG();
	return 0;

phase_REPORT_ERROR:
	s->p_.phase = PHASE_REPORT_ERROR;
	return -1;
}
