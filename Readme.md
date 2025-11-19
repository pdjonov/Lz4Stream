# Lz4Stream

This is a small library of streaming LZ4 decoders, one in C and the other in C#.

# lz4_stream

`lz4_stream` is a LZ4 decoder written in C with a zlib-like interface.

## Usage

Using the decoder is fairly easy (if you've used zlib, this is going to be familiar).

1. Include [lz4_stream.h](lz4_stream.h).
2. First, allocate a `lz4_dec_stream_state` (it's big, so take care if you're looking at putting ito on the stack).
3. Call `lz4_dec_stream_init`.
4. While there's data to decode:
  1. Set the stream state's input and output buffers appropriately.
  2. Call `lz4_dec_stream_run`.
5. If necessary, deallocate the `lz4_dec_stream_state` object.

The stream will alter the values of its `in`, `avail_in`, `out`, and `avail_out` fields as it runs. You can monitor its progress by comparing the values in these fields after `lz4_dec_stream_run` returns to what you set them to beforehand.

The decoder **will read as far ahead as it can** in the input stream, even if it has already filled the output buffer. Make sure you don't give it an input buffer that extends past the end of the actual encoded data, as it may generate errors when it attempts to parse them.

All of the decoder state is in the `lz4_dec_stream_state` object. `lz4_dec_stream_init` allocates nothing further. There's nothing to delete or free when you're done. Just deallocate `lz4_dec_stream_state` however is appropriate.

```c
#include "lz4_stream.h"

//...

#define IN_BUF_LEN 4096
#define OUT_BUF_LEN 4096

uint8_t in_buf[IN_BUF_LEN], out_buf[OUT_BUF_LEN];

lz4_dec_stream_state dec;
lz4_dec_stream_init(&dec);

dec.avail_in = 0;

for (;;)
{
    int stat;
    size_t n_in, n_out, n_in_buf_left;

    //top up the input buffer

    in_buf_left = &in_buf[IN_BUF_LEN] - dec.in;
    if (!dec.avail_in || !in_buf_left)
        //decoder's read to the end of the buffer,
        //start refilling it at the beginning
        dec.in = in_buf;

    n_in = read_input_data(dec.in, &in_buf[IN_BUF_LEN] - dec.in);
    dec.avail_in += n_in;

    //set up the output buffer

    dec.out = out_buf;
    dec.avail_out = OUT_BUF_LEN;

    //run the decoder

    stat = lz4_dec_stream_run(&dec);
    if (stat)
        //decode error, can't really recover
        abort();

    //use the output data

    n_out = dec.out - out_buf;
    write_output_data(out_buf, n_out);

    if (!n_in && !n_out)
        //if we've run out of input data and the decoder has
        //produced no output data, then we're done decoding
        break;
}
```

In addition to `lz4_dec_stream_run`, a `lz4_dec_stream_run_dst_uncached` function is also provided. It is completely interchangeable with `lz4_dec_stream_run`, except that it performs much better when the output buffer is in uncahced/write-combined memory. This can come at a (very) small performance cost compared to `lz4_dec_stream_run`.

## Speed, Robustness

This isn't going to match the performance you get with the standard LZ4 implementation decoding an entire block of data in a single run, but it's still nice and quick.

The implementation has not been thoroughly audited for robustness or security. It shouldn't read or write outside the buffers you give it, but it doesn't strictly validate the input stream and there may be cases where it produces corrupt or invalid output without reporting an error.

# Lz4DecoderStream

`Lz4DecoderStream` is a fully managed, verifyably typesafe, and continuously (that is, not block-based) streaming Lz4 decoder. The decoder accepts any readable .NET stream as input, and it is used like any other read-only stream.

## Usage

Getting a decoder set up is fairly trivial:

```cs
using (var inputStream = File.Open(@"C:\SomePath\Test.lz4"))
{
    var decoder = new Lz4DecoderStream(inputStream);

    var buf = new byte[2048];
    for (;;)
    {
        int nRead = decoder.Read(buf, 0, buf.Length);
        if (nRead == 0)
            break;

        //do stuff with buff
    }
}
```

## Resetting the Decoder

The decoder stream has fairly simple internal state, consisting mainly of a large buffer. If you're on a platform that doesn't handle GC pressure very gracefully, a single stream can be reused by resetting it:

```cs
    //to rescan the input
    inputStream.Position = 0;
    decoder.Reset(inputStream);
```

Though the decoder buffers a small amount of input data, it still requests data in very small chunks (it pulls as little as 128 bytes at once), and should not be used with an input stream that has high per-call `Read` overhead. The size of the internal buffer can be altered by changing the `Lz4DecoderStream.InBufLen` constant. The current value (128) is a tradeoff between reducing the number of `Read` calls made on the input stream and avoiding excessive memory-to-memory copies.

## Input Buffering

By default, the decoder will try to read beyond the end of its input. Under normal circumstances, this is not an issue, as the input stream will simply return a smaller number of actual bytes read, and the decoder will then behave accordingly. However, if you are giving it an input stream in which other data follows the block of Lz4-compressed data you're decompressing, you need to also pass in the length of the source data block when you initialize the decoder. This will prevent it from reading into the next data set.

```cs
    var decoder = new Lz4DecoderStream(inputStream, compressedChunkSize);
    decoder.Reset(inputStream, compressedChunkSize);
```

Finally, by default, the decoder doesn't close its input stream when it is closed.

## Speed, Robustness
The decoder is very fast given the constraints it has to work with (no native or unsafe code). It easily beats the speed of `System.IO.Compression.DeflateStream` and its equivalents in the [DotNetZip](http://dotnetzip.codeplex.com/) and [SharpZipLib](http://www.icsharpcode.net/opensource/sharpziplib/) libraries, even when they're working with zip data that's got a higher compression ratio than the equivalent Lz4 encoding, and *particularly* when reading data in many small pieces rather than in large blocks. (This isn't particularly surprising, given that Lz4 was made to decode faster than zlib in the first place. I mainly mention this to show that this implementation has not sacrificed that property.)

However, because the decoder is working with a streaming interface on both ends, and because it's not decoding data a block at a time, it's naturally slower than a straightforward memory-to-memory all-at-once decoder would be. The decoder is meant for systems where non-stream interfaces aren't an option, and where the environment doesn't allow simply loading the whole of the input data, decoding it into a buffer, and then wrapping it in a `MemoryStream`.

That said, this implementation is **not** suitable for use with untrusted data sources. It isn't going to directly cause a security issue (this is verifiable .NET code, no buffer overruns here), however invalid input data *can* result in invalid output or exceptions from the decoder. Don't throw any old random block of data at the decoder, it isn't going to strictly validate it for you.

## Tuning

The decoder can be tuned by commenting or uncommenting the `#define`s at the top of [Lz4DecoderStream.cs](Lz4DecoderStream.cs). The available options are as follows:

* **`CHECK_ARGS`:** Disable this option to skip argument validation in `Read`. This is generally only useful if you make a tremendous number of very small reads on a CPU-constrained platform.
* **`CHECK_EOF`:** Disabling this option disables a number of checks designed to gracefully handle the end of the input stream. This is only safe to use if you're certain you'll never give the decoder a truncated input stream.
* **`LOCAL_SHADOW`:** Enabling this option may result in a speed increase on platforms with poor indirect load and store performance (mainly weaker ARM chips). Enabling this complicates the code, somewhat, and may be a performance loss, so measure carefully.