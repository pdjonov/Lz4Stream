#include "lz4_stream.h"

#include "lz4.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

template <typename Generator>
static void test_runners(Generator generator)
{
	std::vector<uint8_t> input;
	generator(input);

	REQUIRE(input.size() <= INT_MAX);

	std::vector<uint8_t> compressed;
	compressed.resize((std::size_t)LZ4_compressBound((int)input.size()));
	auto compressed_len = LZ4_compress_default((const char*)input.data(), (char*)compressed.data(), (int)input.size(), (int)compressed.size());
	REQUIRE(compressed_len >= 0);
	compressed.resize((std::size_t)compressed_len);

	std::vector<uint8_t> output;
	auto test_runner = [&](int (*stream_run)(lz4_dec_stream_state*))
	{
		auto test_limited = [&](
			std::size_t in_page_limit = SIZE_MAX,
			std::size_t out_page_limit = SIZE_MAX)
		{
			output.clear();
			output.resize(input.size()); //don't leak data through from prior test!

			lz4_dec_stream_state dec;
			lz4_dec_steram_init(&dec);

			dec.in = compressed.data();
			auto in_end = compressed.data() + compressed.size();
			dec.out = output.data();
			auto out_end = output.data() + output.size();

			while (dec.out < out_end)
			{
				dec.avail_in = std::min((std::size_t)(in_end - dec.in), in_page_limit);
				dec.avail_out = std::min((std::size_t)(out_end - dec.out), out_page_limit);

				auto stream_run_ret = stream_run(&dec);
				REQUIRE(stream_run_ret == 0);
			}

			if (output.empty())
			{
				dec.avail_in = (std::size_t)(in_end - dec.in);
				dec.avail_out = (std::size_t)(out_end - dec.out);

				auto stream_run_ret = stream_run(&dec);
				REQUIRE(stream_run_ret == 0);
			}

			REQUIRE(dec.avail_in == 0);
			REQUIRE(dec.in == in_end);
			REQUIRE(dec.avail_out == 0);
			REQUIRE(dec.out == out_end);

			for (std::size_t i = 0; i < input.size(); i++)
				REQUIRE(input[i] == output[i]);
		};

		test_limited();
		if (input.size() > 1024)
			test_limited(1024, SIZE_MAX);
		if (output.size() > 1024)
			test_limited(SIZE_MAX, 1024);
	};

	SECTION("lz4_dec_steram_run")
	{
		test_runner(lz4_dec_steram_run);
	}

	SECTION("lz4_dec_stream_run_dst_uncached")
	{
		test_runner(lz4_dec_stream_run_dst_uncached);
	}
}

TEST_CASE("empty buffer")
{
	test_runners([](auto& input) { });
}

TEST_CASE("14 zeroes")
{
	test_runners([](auto& input) { input.resize(14, (uint8_t)0); });
}

TEST_CASE("256 zeroes")
{
	test_runners([](auto& input) { input.resize(256, (uint8_t)0); });
}

TEST_CASE("0x40000 zeroes")
{
	test_runners([](auto& input) { input.resize(0x40000, (uint8_t)0); });
}

TEST_CASE("Xorshift noise")
{
	test_runners([](auto& input)
	{
		input.reserve(0x40000);

		//https://en.wikipedia.org/wiki/Xorshift

		std::uint32_t n = 0xDEADBEEF;
		while (input.size() < 0x40000)
		{
			n ^= n << 13;
			n ^= n >> 17;
			n ^= n << 5;

			input.push_back((uint8_t)(n >> 0));
			input.push_back((uint8_t)(n >> 8));
			input.push_back((uint8_t)(n >> 16));
			input.push_back((uint8_t)(n >> 24));
		}
	});
}