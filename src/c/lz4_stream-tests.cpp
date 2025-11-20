#include "lz4_stream.h"

#include "lz4.h"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstdint>
#include <vector>

template <typename Generator>
struct test_data
{
	std::vector<uint8_t> input;
	std::vector<uint8_t> compressed;

	test_data()
	{
		Generator{}(input);
		assert(input.size() <= INT_MAX);

		compressed.resize((std::size_t)LZ4_compressBound((int)input.size()));
		auto compressed_len = LZ4_compress_default((const char*)input.data(), (char*)compressed.data(), (int)input.size(), (int)compressed.size());
		assert(compressed_len >= 0);
		compressed.resize((std::size_t)compressed_len);
	}

	//move the test data out to globals to take setup out of the test execution timings
	static const test_data instance;
};

template <typename Generator>
/* static */ const test_data<Generator> test_data<Generator>::instance{};

template <typename Generator>
static void test_runners()
{
	auto& [input, compressed] = test_data<Generator>::instance;

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
			lz4_dec_stream_init(&dec);

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

			if (std::memcmp(input.data(), output.data(), input.size()) != 0)
				for (std::size_t i = 0; i < input.size(); i++) //this loop ain't as fast as memcmp
					if (input[i] != output[i]) //REQUIE is sloooooooooooow
						REQUIRE(i != i);
		};

		SECTION("one shot")
		{
			test_limited();
		}

		if (input.size() > 1024)
			SECTION("1K read")
			{
				test_limited(1024, SIZE_MAX);
			}

		if (input.size() > 1024)
			SECTION("1K write")
			{
				test_limited(SIZE_MAX, 1024);
			}

		if (input.size() > 512)
			SECTION("512B read")
			{
				test_limited(512, SIZE_MAX);
			}

		if (input.size() > 512)
			SECTION("512B write")
			{
				test_limited(SIZE_MAX, 512);
			}
	};

	SECTION("base")
	{
		test_runner(lz4_dec_stream_run);
	}

	SECTION("dst_uncached")
	{
		test_runner(lz4_dec_stream_run_dst_uncached);
	}
}

template <std::size_t N, uint8_t Val = 0>
struct constant_span
{
	void operator()(std::vector<uint8_t>& input) const
	{
		input.resize(input.size() + N, Val);
	}
};

template <uint8_t Start, uint8_t End>
struct counting_span
{
	void operator()(std::vector<uint8_t>& input) const
	{
		if constexpr (Start < End)
		{
			input.reserve(input.size() + (End - Start) + 1);
			for (uint8_t i = Start; i != End; i++)
				input.push_back(i);
		}
		else if constexpr (End < Start)
		{
			input.reserve(input.size() + (Start - End) + 1);
			for (uint8_t i = Start; i != End; i--)
				input.push_back(i);

		}

		input.push_back(End);
	}
};

template <std::size_t N, std::uint32_t Seed = 0xDEADBEEF>
struct xorshift_uints
{
	void operator()(std::vector<uint8_t>& input) const
	{
		input.reserve(input.size() + N * 4);

		//https://en.wikipedia.org/wiki/Xorshift

		std::uint32_t n = Seed;
		for (std::size_t i = 0; i < N; i++)
		{
			n ^= n << 13;
			n ^= n >> 17;
			n ^= n << 5;

			input.push_back((uint8_t)(n >> 0));
			input.push_back((uint8_t)(n >> 8));
			input.push_back((uint8_t)(n >> 16));
			input.push_back((uint8_t)(n >> 24));
		}
	}
};

template <typename... Ts>
struct chained_generators
{
	void operator()(std::vector<uint8_t>& input) const
	{
		(Ts{}(input),...);
	}
};

template <typename Gen, std::size_t Reps>
struct repeated_generator
{
	void operator()(std::vector<uint8_t>& input) const
	{
		for (std::size_t i = 0; i < Reps; i++)
			Gen{}(input);
	}
};

TEST_CASE("empty buffer")
{
	test_runners<constant_span<0>>();
}

TEST_CASE("14 zeroes")
{
	test_runners<constant_span<14>>();
}

TEST_CASE("256 zeroes")
{
	test_runners<constant_span<256>>();
}

TEST_CASE("0x40000 zeroes")
{
	test_runners<constant_span<0x40000>>();
}

TEST_CASE("0x400000 zeroes")
{
	test_runners<constant_span<0x400000>>();
}

TEST_CASE("small RLEs")
{
	test_runners<
		chained_generators<
			repeated_generator<counting_span<1,  2>, 256>,
			repeated_generator<counting_span<1,  3>, 256>,
			repeated_generator<counting_span<1,  4>, 256>,
			repeated_generator<counting_span<1,  5>, 256>,
			repeated_generator<counting_span<1,  6>, 256>,
			repeated_generator<counting_span<1,  7>, 256>,
			repeated_generator<counting_span<1,  8>, 256>,
			repeated_generator<counting_span<1,  9>, 256>,
			repeated_generator<counting_span<1, 10>, 256>,
			repeated_generator<counting_span<1, 11>, 256>,
			repeated_generator<counting_span<1, 12>, 256>,
			repeated_generator<counting_span<1, 13>, 256>,
			repeated_generator<counting_span<1, 14>, 256>,
			repeated_generator<counting_span<1, 15>, 256>,
			repeated_generator<counting_span<1, 16>, 256>,
			repeated_generator<counting_span<1, 17>, 256>,
			repeated_generator<counting_span<1, 18>, 256>,
			repeated_generator<counting_span<1, 19>, 256>,
			repeated_generator<counting_span<1, 20>, 256>,
			repeated_generator<counting_span<1, 21>, 256>,
			repeated_generator<counting_span<1, 22>, 256>,
			repeated_generator<counting_span<1, 23>, 256>,
			repeated_generator<counting_span<1, 24>, 256>,
			repeated_generator<counting_span<1, 25>, 256>,
			repeated_generator<counting_span<1, 26>, 256>,
			repeated_generator<counting_span<1, 27>, 256>,
			repeated_generator<counting_span<1, 28>, 256>,
			repeated_generator<counting_span<1, 29>, 256>,
			repeated_generator<counting_span<1, 30>, 256>,
			repeated_generator<counting_span<1, 31>, 256>,
			repeated_generator<counting_span<1, 32>, 256>,
			repeated_generator<counting_span<1, 33>, 256>,
			repeated_generator<counting_span<1, 34>, 256>,
			repeated_generator<counting_span<1, 35>, 256>,
			repeated_generator<counting_span<1, 36>, 256>,
			repeated_generator<counting_span<1, 37>, 256>,
			repeated_generator<counting_span<1, 38>, 256>,
			repeated_generator<counting_span<1, 39>, 256>,
			repeated_generator<counting_span<1, 40>, 256>,
			repeated_generator<counting_span<1, 41>, 256>,
			repeated_generator<counting_span<1, 42>, 256>,
			repeated_generator<counting_span<1, 43>, 256>,
			repeated_generator<counting_span<1, 44>, 256>,
			repeated_generator<counting_span<1, 45>, 256>,
			repeated_generator<counting_span<1, 46>, 256>,
			repeated_generator<counting_span<1, 47>, 256>,
			repeated_generator<counting_span<1, 48>, 256>,
			repeated_generator<counting_span<1, 49>, 256>,
			repeated_generator<counting_span<1, 50>, 256>,
			repeated_generator<counting_span<1, 51>, 256>,
			repeated_generator<counting_span<1, 52>, 256>,
			repeated_generator<counting_span<1, 53>, 256>,
			repeated_generator<counting_span<1, 54>, 256>,
			repeated_generator<counting_span<1, 55>, 256>,
			repeated_generator<counting_span<1, 56>, 256>,
			repeated_generator<counting_span<1, 57>, 256>,
			repeated_generator<counting_span<1, 58>, 256>,
			repeated_generator<counting_span<1, 59>, 256>,
			repeated_generator<counting_span<1, 60>, 256>,
			repeated_generator<counting_span<1, 61>, 256>,
			repeated_generator<counting_span<1, 62>, 256>,
			repeated_generator<counting_span<1, 63>, 256>,
			
			repeated_generator<counting_span<1, 64>, 256>,
			repeated_generator<counting_span<1, 65>, 256>,
			repeated_generator<counting_span<1, 66>, 256>,
			repeated_generator<counting_span<1, 67>, 256>,

			repeated_generator<counting_span<1, 255>, 256>
		>>();
}

TEST_CASE("Xorshift noise")
{
	test_runners<xorshift_uints<0x10000>>();
}

TEST_CASE("big mixed")
{
	test_runners<
		chained_generators<
			xorshift_uints<0x10000/4>,
			constant_span<0x1000>,
			xorshift_uints<0x10000/8>,
			constant_span<0x1000, 0xF0>,
			xorshift_uints<0x10000, 0xBAADCAFE>,
			repeated_generator<
				chained_generators<
					counting_span<40, 255>,
					counting_span<132, 0>,
					counting_span<60, 140>
				>, 128>,
			constant_span<0x10000, 0x0F>,
			repeated_generator<
				chained_generators<
					counting_span<0, 255>,
					xorshift_uints<0x100000>,
					counting_span<255, 0>
				>, 4>,
			constant_span<0x10000, 0xBA>
		>
	>();
}

TEST_CASE("many matches")
{
	test_runners<
		repeated_generator<
				chained_generators<
					counting_span<0, 255>,
					counting_span<255, 0>
				>, 8 * 1024>
	>();
}