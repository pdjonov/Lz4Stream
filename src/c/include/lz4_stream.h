#ifndef LZ4_STREAM_H
#define LZ4_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
	Usage:

	1.	Allocate an instance of lz4_dec_stream_state (anywhere,
		on the stack if you like) and call lz4_dec_stream_init
		to initialize its fields.

	2.	Set the in and avail_in fields to point to your input
		data. Providing only partial input data is allowed.

	3.	Set the out and avail_out fields to point to your
		output buffer. The buffer need not be large enough to
		hold the entire result.

	4.	Call lz4_dec_stream_run. It will decode as much as it
		can before it either runs out of input data or fills
		the entire output buffer. If lz4_dec_stream_run returns
		a nonzero value, it indicates an error, and you should
		abort decoding.

	5.	Repeat steps 2-4 as needed.

	6.	Deallocate the lz4_dec_stream_state object. If you wish
		to reuse it, simply call lz4_dec_stream_init again. It
		holds no external resources, so it's safe to deal with
		it any way you like.

	lz4_dec_stream_run updates the in, avail_in, out, and avail_out
	fields as it works, similarly to zlib's streaming interface.
	You can track the progress of the stream by watching how these
	values change across a call.

	The input and output blocks must not overlap.
*/

typedef struct lz4_dec_stream_state
{
	const uint8_t		*in;
	size_t				avail_in;

	uint8_t				*out;
	size_t				avail_out;

	//private state - no touchy!

	struct
	{
#ifdef __cplusplus
		alignas(uintptr_t)
#else
		_Alignas(uintptr_t) //should already be true, but juuuuuust in case
#endif
		uint8_t			o_buf[0x10000];

		unsigned int	lit_len, mat_len;
		unsigned int	o_pos, mat_dst;
		unsigned int	phase;
	} p_;
} lz4_dec_stream_state;

void lz4_dec_stream_init(lz4_dec_stream_state *s);
int lz4_dec_stream_run(lz4_dec_stream_state *s);
int lz4_dec_stream_run_dst_uncached(lz4_dec_stream_state *s);

#ifdef __cplusplus
}
#endif

#endif
