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

#define O_BUF_LEN				(sizeof( ((lz4_dec_stream_state*)0)->p_.o_buf ))

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