#include "lz4_stream.h"

#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stdlib.h>

#define PHASE_READ_TOK			0
#define PHASE_READ_EX_LIT_LEN	1
#define PHASE_COPY_LIT			2
#define PHASE_READ_OFS			3
#define PHASE_READ_OFS2			4
#define PHASE_READ_EX_MAT_LEN	5
#define PHASE_COPY_MAT			6

#define PHASE_REPORT_ERROR		7

#define MAX_BLOCK_LEN			UINT_MAX

#define O_BUF_LEN 				sizeof(((lz4_dec_stream_state*)0)->p_.o_buf)

_Static_assert((O_BUF_LEN & (O_BUF_LEN - 1)) == 0, "o_buf not pow2 size; fix below");
#define WRAP_OBUF_IDX(idx) 		((idx) & (O_BUF_LEN - 1))

void lz4_dec_stream_init( lz4_dec_stream_state *s )
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

int lz4_dec_stream_run( lz4_dec_stream_state *s )
{
	//pull everything into locals

	const uint8_t *in = s->in;
	const uint8_t *in_end = s->in + s->avail_in;

	uint8_t *out_start = s->out;
	uint8_t *out = s->out;
	size_t avail_out = s->avail_out;

	uint8_t *o_buf = s->p_.o_buf;
	unsigned int o_pos = s->p_.o_pos;
	
	unsigned int lit_len = s->p_.lit_len;
	unsigned int mat_len = s->p_.mat_len;
	unsigned int mat_dst = s->p_.mat_dst;

	unsigned int phase = s->p_.phase;

	unsigned int c;
	size_t len;

	//jump off into the loop

	switch( phase )
	{
	case PHASE_READ_TOK:			goto p_read_tok;
	case PHASE_READ_EX_LIT_LEN:		goto p_read_ex_lit_len;
	case PHASE_COPY_LIT:			goto p_copy_lit;
	case PHASE_READ_OFS:			goto p_read_ofs;
	case PHASE_READ_OFS2:			goto p_read_ofs2;
	case PHASE_READ_EX_MAT_LEN:		goto p_read_ex_mat_len;
	case PHASE_COPY_MAT:			goto p_copy_mat;
	case PHASE_REPORT_ERROR:		return -1;

	default:
		assert( 0 && "corrupt decoder stream state" );
#if _MSC_VER
		//this is a programmer error, not a result of bad input
		//let the compiler hoist out the check in release builds
		__assume( 0 );
#else
		//the return isn't included in MSVC builds since the __assume( 0 )
		//annotation (correctly) triggers "unreachable code" warnings
		return -1;
#endif
	}

p_read_tok:
	if( in == in_end )
		goto finish;

	c = *in++;

	lit_len = c >> 4;
	mat_len = (c & 0xF) + 4;

	switch( lit_len )
	{
	case 0:
		phase = PHASE_READ_OFS;
		goto p_read_ofs;

	case 0xF:
		phase = PHASE_READ_EX_LIT_LEN;
		goto p_read_ex_lit_len;

	default:
		phase = PHASE_COPY_LIT;
		goto p_copy_lit;
	}

p_read_ex_lit_len:
	if( in == in_end )
		goto finish;

	c = *in++;
	if( c > MAX_BLOCK_LEN - lit_len )
		goto error;

	lit_len += c;
	if( c == 255 )
		goto p_read_ex_lit_len;

	phase = PHASE_COPY_LIT;
	goto p_copy_lit;

p_copy_lit:
	len = in_end - in;
	if( len > lit_len ) len = lit_len;
	if( len > avail_out ) len = avail_out;

	if( len )
	{
		memcpy( out, in, len );

		in += len;
		out += len;
		avail_out -= len;
		lit_len -= len;
	}

	if( lit_len )
		//ran out of input or output space, try again later
		goto finish;

	phase = PHASE_READ_OFS;
	goto p_read_ofs;

p_read_ofs:
	if( in == in_end )
		goto finish;

	mat_dst = *in++;

	phase = PHASE_READ_OFS2;
	goto p_read_ofs2;

p_read_ofs2:
	if( in == in_end )
		goto finish;

	mat_dst |= *in++ << 8;

	if( !mat_dst )
		goto error;

	if( mat_len == 15 + 4 )
	{
		phase = PHASE_READ_EX_MAT_LEN;
		goto p_read_ex_mat_len;
	}
	else
	{
		phase = PHASE_COPY_MAT;
		goto p_copy_mat;
	}

p_read_ex_mat_len:
	if( in == in_end )
		goto finish;

	c = *in++;
	if( c > MAX_BLOCK_LEN - mat_len )
		goto error;

	mat_len += c;
	if( c == 255 )
		goto p_read_ex_mat_len;

	phase = PHASE_COPY_MAT;
	goto p_copy_mat;

p_copy_mat:
	len = mat_len < avail_out ? mat_len : avail_out;
	if( len )
	{
		size_t n_read = out - out_start;
		if( mat_dst > n_read )
		{
			//we're reading far enough back that we need to hit the buffer
			
			size_t e;

			//figure out how far back into the buffer we need to go
			size_t buf_dst = mat_dst - n_read;
			//and how many bytes we'll pull from it
			size_t buf_cnt = buf_dst < len ? buf_dst : len;

			//and exactly where in the buffer we'll copy from
			int buf_src = (int)o_pos - buf_dst;
			if( buf_src < 0 )
				buf_src += O_BUF_LEN;

			e = (size_t)buf_src + buf_cnt;
			if( e > O_BUF_LEN )
			{
				e = O_BUF_LEN - buf_src;
				memcpy( out, o_buf + buf_src, e );
				memcpy( out + e, o_buf, buf_cnt - e );
			}
			else
			{
				memcpy( out, o_buf + buf_src, buf_cnt );
			}

			out += buf_cnt;
			avail_out -= buf_cnt;

			len -= buf_cnt;
			mat_len -= buf_cnt;
		}

		{
			size_t c = len;
			const uint8_t *out_src = out - mat_dst;
			while( c-- )
				*out++ = *out_src++;
		}

		avail_out -= len;
		mat_len -= len;
	}

	if( mat_len )
		goto finish;

	phase = PHASE_READ_TOK;
	goto p_read_tok;

finish:
	//tuck everything away for the next call

	len = out - out_start;
	if( len >= O_BUF_LEN )
	{
		memcpy( o_buf, out - O_BUF_LEN, O_BUF_LEN );
		o_pos = 0;
	}
	else
	{
		size_t e = o_pos + len;
		if( e > O_BUF_LEN )
		{
			e = O_BUF_LEN - o_pos;
			memcpy( o_buf + o_pos, out - len, e );

			o_pos = len - e;
			memcpy( o_buf, out - o_pos, o_pos );
		}
		else
		{
			memcpy( o_buf + o_pos, out - len, len );
			o_pos += len;
			if( o_pos == O_BUF_LEN ) o_pos = 0;
		}
	}

	s->in = in;
	s->avail_in = in_end - in;

	s->out = out;
	s->avail_out = avail_out;

	s->p_.lit_len = lit_len;
	s->p_.mat_len = mat_len;
	s->p_.o_pos = o_pos;
	s->p_.mat_dst = mat_dst;

	s->p_.phase = phase;

	return 0;

error:
	s->p_.phase = PHASE_REPORT_ERROR;
	return -1;
}

int lz4_dec_stream_run_dst_uncached(lz4_dec_stream_state* s)
{
	//work with stack variables, write the updated values back before returning

	const uint8_t* restrict in = s->in;
	const uint8_t* restrict const in_end = s->in + s->avail_in;

	uint8_t* restrict out = s->out;
	size_t avail_out = s->avail_out;

	uint8_t* restrict const o_buf = s->p_.o_buf;
	unsigned int o_pos = s->p_.o_pos;

	unsigned int lit_len = s->p_.lit_len; //the length of the current literal
	unsigned int mat_len = s->p_.mat_len; //the length of the current match
	unsigned int mat_dst = s->p_.mat_dst; //the distance to the current match

	unsigned int phase = s->p_.phase;
	switch (phase)
	{
	case PHASE_READ_TOK: 		goto phase_READ_TOK;
	case PHASE_READ_EX_LIT_LEN:	goto phase_READ_EX_LIT_LEN;
	case PHASE_COPY_LIT:		goto phase_COPY_LIT;
	case PHASE_READ_OFS:		goto phase_READ_OFS;
	case PHASE_READ_OFS2:		goto phase_READ_OFS2;
	case PHASE_READ_EX_MAT_LEN:	goto phase_READ_EX_MAT_LEN;
	case PHASE_COPY_MAT:		goto phase_COPY_MAT;
	case PHASE_REPORT_ERROR: 	goto phase_REPORT_ERROR;

	default:
		assert(0 && "corrupt decoder stream state");

		//this is a programmer error, not a result of bad input
		//let the compiler hoist out the check in release builds

#if defined(_MSC_VER)
		__assume(0);
#elif defined(__GNUG__)
		__builtin_unreachable();
#else
		goto phase_REPORT_ERROR;
#endif
	}

#define TRANSITION_TO_PHASE(next_phase) \
	if (1) { phase = PHASE_##next_phase; goto phase_##next_phase; } else ((void)0)
#define SUSPEND_FOR_NOW() \
	goto suspend_for_now
#define SUSPEND_IF_INPUT_EMPTY() \
	if (in == in_end) SUSPEND_FOR_NOW(); else ((void)0)

phase_READ_TOK:
	SUSPEND_IF_INPUT_EMPTY();

	{
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

phase_READ_EX_LIT_LEN: //loop
	//we're in the middle of reading a long literal length
	SUSPEND_IF_INPUT_EMPTY();

	{
		uint8_t c = *in++;

		if (c > MAX_BLOCK_LEN - lit_len)
			TRANSITION_TO_PHASE(REPORT_ERROR);

		lit_len += c;

		if (c == 0xFF)
			goto phase_READ_EX_LIT_LEN; //loop
	}

	TRANSITION_TO_PHASE(COPY_LIT);

phase_COPY_LIT:
	//copy lit_len bytes from the input to the output

	{
		unsigned int clamped_lit_len = lit_len;

		size_t avail_in = in_end - in;
		if (clamped_lit_len > avail_in)
			clamped_lit_len = (unsigned int)avail_in;
		if (clamped_lit_len > avail_out)
			clamped_lit_len = (unsigned int)avail_out;

		for (unsigned int i = 0; i < clamped_lit_len; i++)
		{
			uint8_t c = *in++;

			o_buf[o_pos] = c;
			o_pos = WRAP_OBUF_IDX(o_pos + 1);

			*out++ = c;
		}

		avail_out -= clamped_lit_len;
		lit_len -= clamped_lit_len;
	}

	if (lit_len)
		//there's more literal to copy, but either src or dst bufs ran out
		SUSPEND_FOR_NOW();

	TRANSITION_TO_PHASE(READ_OFS);

phase_READ_OFS:
	SUSPEND_IF_INPUT_EMPTY();

	mat_dst = *in++;

	TRANSITION_TO_PHASE(READ_OFS2);

phase_READ_OFS2:
	SUSPEND_IF_INPUT_EMPTY();

	mat_dst |= (unsigned int)*in++ << 8;

	if (!mat_dst)
		TRANSITION_TO_PHASE(REPORT_ERROR);

	if (mat_len == 0xF + 4)
		TRANSITION_TO_PHASE(READ_EX_MAT_LEN);
	else
		TRANSITION_TO_PHASE(COPY_MAT);

	_Static_assert(0xFFFF < O_BUF_LEN, "mat_dst must never reach beyond o_buf");

phase_READ_EX_MAT_LEN: //loop
	SUSPEND_IF_INPUT_EMPTY();

	{
		uint8_t c = *in++;

		if (c > MAX_BLOCK_LEN - mat_len)
			TRANSITION_TO_PHASE(REPORT_ERROR);

		mat_len += c;

		if (c == 0xFF)
			goto phase_READ_EX_MAT_LEN; //loop
	}

	TRANSITION_TO_PHASE(COPY_MAT);

phase_COPY_MAT:
	//copy mat_len bytes from mat_dst bytes behind the output cursor
	//dst_bytes will not be more than O_BUF_LEN = sizeof(s->p_.o_buf)
	{
		unsigned int clamped_mat_len = mat_len;
		if (clamped_mat_len > avail_out)
			clamped_mat_len = (unsigned int)avail_out;

		unsigned int o_inpos = WRAP_OBUF_IDX(o_pos - mat_dst);
		for (unsigned int i = 0; i < clamped_mat_len; i++)
		{
			uint8_t c = o_buf[o_inpos];
			o_inpos = WRAP_OBUF_IDX(o_inpos + 1);

			o_buf[o_pos] = c;
			o_pos = WRAP_OBUF_IDX(o_pos + 1);

			*out++ = c;
		}

		avail_out -= clamped_mat_len;
		mat_len -= clamped_mat_len;
	}

	if (mat_len)
		//we ran out of avail_out before we finished
		SUSPEND_FOR_NOW();

	TRANSITION_TO_PHASE(READ_TOK);

suspend_for_now:
	//tuck everything away for the next call

	s->in = in;
	s->avail_in = in_end - in;

	s->out = out;
	s->avail_out = avail_out;

	s->p_.lit_len = lit_len;
	s->p_.mat_len = mat_len;
	s->p_.o_pos = o_pos;
	s->p_.mat_dst = mat_dst;

	s->p_.phase = phase;

	return 0;

phase_REPORT_ERROR:
	s->p_.phase = PHASE_REPORT_ERROR;
	return -1;
}