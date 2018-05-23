#include "cxxenvi.hh"

void second(size_t rows, size_t cols)
{
	std::vector<float> what;
	what.resize(rows*cols);

	for (size_t r = 0; r < rows; ++r)
		for (size_t c = 0; c < cols; ++c)
			what[r*cols+c] = c - r;

	ENVI::dump("/tmp/hm", "hm", rows, cols, what);
}
