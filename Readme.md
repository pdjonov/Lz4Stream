#Lz4Stream

This is a small library of managed Lz4-related code.

##Lz4DecoderStream

`Lz4DecoderStream` is a fully managed, verifyably typesafe, and continuously (that is, not block-based) streaming
Lz4 decoder. The decoder accepts any readable .NET stream as input, and it is used like any other read-only stream.

###Usage

Getting a decoder set up is fairly trivial:

```cs
using( var inputStream = File.Open( @"C:\SomePath\Test.lz4" ) )
{
    var decoder = new Lz4DecoderStream( inputStream );

    var buf = new byte[2048];
    for( ; ; )
    {
        int nRead = decoder.Read( buf, 0, buf.Length );
        if( nRead == 0 )
            break;
            
        //do stuff with buff
    }
}
```

###Resetting the Decoder

The decoder stream has fairly simple internal state, consisting mainly of a large buffer. If you're on a platform
that doesn't handle GC pressure very gracefully, a single stream can be reused by resetting it:

```cs
    //to rescan the input
    inputStream.Position = 0;
    decoder.Reset( inputStream );
```

Though the decoder buffers a small amount of input data, it still requests data in very small chunks (it pulls as
little as 128 bytes at once), and should not be used with an input stream that has high per-call `Read` overhead.
The size of the internal buffer can be altered by changing the `Lz4DecoderStream.InBufLen` constant. The current
value (128) is a tradeoff between reducing the number of `Read` calls made on the input stream and avoiding
excessive memory-to-memory copies.

###Input Buffering

By default, the decoder will try to read beyond the end of its input. Under normal circumstances, this is not an
issue, as the input stream will simply return a smaller number of actual bytes read, and the decoder will then
behave accordingly. However, if you are giving it an input stream in which other data follows the block of
Lz4-compressed data you're decompressing, you need to also pass in the length of the source data block when you
initialize the decoder. This will prevent it from reading into the next data set.

```cs
    var decoder = new Lz4DecoderStream( inputStream, compressedChunkSize );
    decoder.Reset( inputStream, compressedChunkSize );
```

Finally, by default, the decoder doesn't close its input stream when it is closed.

###Speed, Robustness
The decoder is very fast given the constraints it has to work with (no native or unsafe code). It easily beats the
speed of `System.IO.Compression.DeflateStream` and its equivalents in the [DotNetZip](http://dotnetzip.codeplex.com/)
and [SharpZipLib](http://www.icsharpcode.net/opensource/sharpziplib/) libraries, *particularly* when reading data
in many small pieces rather than in large blocks.

That said, this implementation is **not** suitable for use with untrusted data sources. It isn't going to directly
cause a security issue (this is verifiable .NET code, no buffer overruns here), however invalid input data *can*
result in invalid output or exceptions from the decoder. Don't throw any old random block of data at the decoder,
it isn't going to strictly validate it for you.

###Tuning

The decoder can be tuned by commenting or uncommenting the `#define`s at the top of [Lz4DecoderStream.cs](Lz4DecoderStream.cs).
The available options are as follows:

* **`CHECK_ARGS`:** Disable this option to skip argument validation in `Read`. This is generally only useful if you
  make a tremendous number of very small reads on a CPU-constrained platform.
* **`CHECK_EOF`:** Disabling this option disables a number of checks designed to gracefully handle the end of the
  input stream. This is only safe to use if you're certain you'll never give the decoder a truncated input stream.
* **`LOCAL_SHADOW`:** Enabling this option may result in a speed increase on platforms with poor indirect load and
  store performance (mainly weaker ARM chips). Enabling this complicates the code, somewhat, and may be a performance
  loss, so measure carefully.