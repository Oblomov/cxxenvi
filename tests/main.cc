#include <vector>
#include <cassert>
#include <iostream>

#include "cxxenvi.hh"

int main(void) // (int argc, char *argv[]) // TODO for testing
{
	const size_t rows = 32, cols = 64;

	std::vector<float> what;
	what.resize(rows*cols);

	for (size_t r = 0; r < rows; ++r)
		for (size_t c = 0; c < cols; ++c)
			what[r*cols+c] = c - r;

	ENVI::dump("/tmp/hm", "hm", rows, cols, what);

	auto e = ENVI::create<float>("/tmp/hm2", "hm2", rows, cols/2);

	e->add_channel_rect("hm", what, cols, 0, 16);
	e->add_meta("map info", "UTM", 1, 1, 5e5, 4e6, 30, 30, 33, "North", "WGS-84");

	e.reset();

	auto r = ENVI::ropen("/tmp/hm2");
	size_t nrows, ncols;
	std::vector<float> read;
	r->get_channel(0, nrows, ncols, read);
	assert(nrows == rows);
	assert(ncols == cols/2);
	for (size_t r = 0; r < nrows; ++r)
		for (size_t c = 0; c < ncols; ++c)
			assert(read[r*ncols+c] == c + 16 - r);

	auto w = r->get_meta_tuple<std::string,
		int, int, float, float, float, float, int,
		std::string, std::string>("map info");
	assert(std::get<0>(w) == "UTM");
	assert(std::get<1>(w) == 1);
	assert(std::get<2>(w) == 1);
	assert(std::get<3>(w) == 5e5);
	assert(std::get<4>(w) == 4e6);
	assert(std::get<5>(w) == 30);
	assert(std::get<6>(w) == 30);
	assert(std::get<7>(w) == 33);
	assert(std::get<8>(w) == "North");
	assert(std::get<9>(w) == "WGS-84");

	auto wless = r->get_meta_tuple<std::string, int, int>("map info");
	assert(std::get<0>(wless) == "UTM");
	assert(std::get<1>(wless) == 1);
	assert(std::get<2>(wless) == 1);

	auto wmore = r->get_meta_tuple<std::string,
		int, int, float, float, float, float, int,
		std::string, std::string, std::string>("map info");
	assert(std::get<0>(wmore) == "UTM");
	assert(std::get<1>(wmore) == 1);
	assert(std::get<2>(wmore) == 1);
	assert(std::get<3>(wmore) == 5e5);
	assert(std::get<4>(wmore) == 4e6);
	assert(std::get<5>(wmore) == 30);
	assert(std::get<6>(wmore) == 30);
	assert(std::get<7>(wmore) == 33);
	assert(std::get<8>(wmore) == "North");
	assert(std::get<9>(wmore) == "WGS-84");
	assert(std::get<10>(wmore).empty());

	{
		int row, col;
		float lat, lon;
		int vres, hres;
		r->get_meta_tuple("map info",
			std::ignore, row, col, lat, lon, vres, hres);
		assert(row == 1);
		assert(col == 1);
		assert(lat == 5e5);
		assert(lon == 4e6);
		assert(vres == 30);
		assert(hres == 30);
	}

};
