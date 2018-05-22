#ifndef CXXENVI_HH
#define CXXENVI_HH

/* Read/Write raw ENVI files: these are split into a 'raw'
 * file plus a separate .hdr file.
 * TODO:
 * 	since the .hdr supports offsets, we could make the 'raw' file into
 * 	some other format with included header. Sadly we can't use PFM,
 * 	whose scanlines are stored bottom to top. Maybe FITS?
 * TODO:
 * 	support reading other raw formats (ESRI, or what Gdal calls 'GenBin'),
 * 	they work in the same way, just the header is marginally different.
 * TODO:
 * 	support arbitrary streams for input/ouput (WIP)
 * TODO:
 * 	support other raw interleave formats (BIL and BIP)
 * TODO:
 * 	while ENVI doesn't seem to have builtin support for compressed files,
 * 	supporting them at least with BSQ would be quite trivial, by either using
 * 	something like gzstream or zlib directly. The latter would even allow us to
 * 	compress each channel separately, allowing easier seeks on load.
 * TODO:
 * 	inline all we can, check performance
 * TODO:
 * 	improve documentation detail and quality
 */

/*
  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */


/*** Configuration ***/

// To enable support for complex types, define CXXENVI_COMPLEX to any
// non-zero value before including this header. By default we disable
// support to avoid loading unnecessary files
#ifndef CXXENVI_COMPLEX
#define CXXENVI_COMPLEX 0
#endif

// To enable output of (some) diagnostic information, define CXXENVI_DEBUG
// to any non-zero value before including this header. By default we
// disable any output
#ifndef CXXENVI_DEBUG
#define CXXENVI_DEBUG 0
#endif

/*
 * Standard includes
 */

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <fstream>
#include <functional>
#include <sstream>
#include <memory>
#include <algorithm>

#if CXXENVI_COMPLEX
#include <complex>
#endif

#if CXXENVI_DEBUG
#include <iostream>
#endif

class ENVI
{
public:
	/*
	 * Whitespace functions
	 */

	/* We will need to trim whitespace, and sadly the standard
	 * library doesn't have functions for that, so we will have to
	 * define our own. Make them public for convenience of others.
	 */

	// remove whitespace on the right
	static inline std::string&
	rtrim(std::string &str, const char *whitespace = " \n\t\v")
	{
		size_t nws = str.find_last_not_of(whitespace);
		if (nws == str.npos)
			str.clear();
		else
			str.erase(++nws, str.npos);
		return str;
	}

	// remove whitespace on the left
	static inline std::string&
	ltrim(std::string &str, const char *whitespace = " \n\t\v")
	{
		size_t nws = str.find_first_not_of(whitespace);
		if (nws == str.npos)
			str.clear();
		else
			str.erase(0, nws);
		return str;
	}

	// remove whitespace on both sides
	static inline std::string&
	trim(std::string &str, const char *whitespace = " \n\t\v")
	{
		return ltrim(rtrim(str, whitespace), whitespace);
	}

	/*
	 * Tuple functions
	 */

	/* ENVI files can contain metadata values with multiple
	 * types. We allow fetching them as a vector of strings, or
	 * as a tuple of values of different types. The function(s)
	 * to convert from vector of strings to tuple is very useful,
	 * so we expose it too
	 */


	template<size_t pos, typename TupleType>
	static
	typename std::enable_if<(pos == std::tuple_size<TupleType>::value)>::type
	string_to_tuple(
		std::vector<std::string> const& /* strings */,
		TupleType & /* tuple */)
	{
		return;
	}

	template<typename T>
	static void string_extract(std::string const& str, T& out)
	{
		std::stringstream ss(str);
		ss >> out;
	}

	// Specialization for std::ignore defined outside

	template<size_t pos, typename TupleType>
	static
	typename std::enable_if<(pos < std::tuple_size<TupleType>::value)>::type
	string_to_tuple(
		std::vector<std::string> const& strings,
		TupleType &tuple)
	{
		if (pos < strings.size()) {
			string_extract(strings[pos], std::get<pos>(tuple));
			string_to_tuple<pos+1>(strings, tuple);
		}
	}

	/*
	 * Endianness
	 */

	// NOTE: ENVI doesn't seem to support mixed-endian hardware
	// such as the PDP-11
	enum Endian
	{
		LITTLE = 0, /* Intel etc */
		BIG = 1 /* Motorola, network byte order, etc */
	};

	constexpr static inline Endian endianness()
	{
		// TODO FIXME this is not guaranteed to work since there
		// is no requirement that the string will be aligned
		// in such a way that it could be reinterpreted as an
		// uint16_t
		return (*reinterpret_cast<const uint16_t*>("\xff\x00") == 0xff) ?
			LITTLE : BIG ;
	}

	/*
	 * Data types
	 */

	enum DataTypeEnum
	{
		CHAR   = 1,
		INT16  = 2,
		INT32  = 3,
		FP32   = 4,
		FP64   = 5,
		FP32C  = 6,
		FP64C  = 9,
		UINT16 = 12,
		UINT32 = 13,
		INT64  = 14,
		UINT64 = 15
	};

	// check if a type code is valid
	constexpr static inline bool
	valid_type(long type)
	{
		return (type >= 1 && type <= 15 &&
			type != 7 &&
			type != 8 &&
			(type <= 9 || type >=12));
	}

	// return the next valid DataTypeEnum. If complex
	// support is enabled, we have to handle discontinuities
	// between FP32C/FP64C and FP64C/UINT16, otherwise
	// we only take care of the gap FP64/UINT16.
	// We use UINT64 itself as 'next type' to UINT64 itself,
	// it's up to the caller to check for repetion and abort
	// as necessary.
	// (Another solution would be to add some invalid type value
	// to the enum, but I'm not sure it would be better.)
	constexpr static inline DataTypeEnum
	next_type(DataTypeEnum cur)
	{
		return (
#if CXXENVI_COMPLEX
			(cur == FP32C) ? FP64C :
			(cur == FP64C) ? UINT16 :
#else
			(cur == FP64) ?  UINT16 :
#endif
			(cur == UINT64) ? UINT64 : // meh
			static_cast<DataTypeEnum>(cur+1)
		       );
	}

	// Forward declaration of a template structure used to convert
	// typenames into the corresponding DataTypeEnum:
	// Example usage: ENVI::TypeCode<float>()
	// To get: ENVI::DataTypeEnum::FP32
	template<typename T> struct TypeCode;

	// Forward declaration of a template structure used to convert
	// DataTypeEnum into the corresponding typenames
	// Example usage: ENVI::CodeType<ENVI::DataTypeEnum::INT64>::type
	// To get: int64_t
	template<DataTypeEnum val> struct CodeType;

private:

	// ENVI replaces the last extension with .hdr, or appends .hdr
	// if no extension is found. We follow the same practice.
	static inline std::string hdr_name(std::string const& fname)
	{
		if (fname.empty())
			throw std::invalid_argument("data filename cannot be empty");
		size_t dot = fname.rfind('.');
		if (dot == fname.size() - 1)
			return fname + "hdr";
		if (dot == fname.npos || dot < 2)
			return fname + ".hdr";
		return fname.substr(0, dot) + ".hdr";
	}

	// The metadata included in a header file: a set of key-values.
	// We want to preserve order, so instead of using a hash
	// we use a pair of vectors
	class Metadata
	{
		static const size_t npos = SIZE_MAX;

		std::vector<std::string> keys;
		std::vector<std::string> values;

		size_t index(std::string const& _k, bool fail_present = false) const
		{
			const auto ini(keys.cbegin());
			const auto fin(keys.cend());
			const auto found(std::find(ini, fin, _k));
			size_t idx = ((found == fin) ? npos : found - ini);

			if (idx != npos && fail_present)
				throw std::runtime_error("key " + _k + " already exists with value " + values[idx]);

			return idx;
		}

		void create_kval(std::string const& _key, std::string const& _val)
		{
			keys.push_back(_key);
			values.push_back(_val);
		}

		template<typename T>
		void create_kval(std::string const& _key, T const& _val)
		{
			std::stringstream str;
			str.precision(16);
			str << _val;
			return create_kval(_key, str.str());
		}

		// Terminator for the variadic template expansion
		void append_values(std::stringstream& ss, size_t /* count */)
		{
			ss << " }";
		}

		template<typename ...T>
		void append_values(std::stringstream& ss, size_t count,
			char const* str, T const& ... rest)
		{
			if (count)
				ss << ", ";
			ss << str;
			append_values(ss, count+1, rest...);
		}

		template<typename T1, typename ...T>
		void append_values(std::stringstream& ss, size_t count,
			T1 const& value, T const& ... rest)
		{
			if (count)
				ss << ", ";
			ss << value;
			append_values(ss, count+1, rest...);
		}

	public:

		size_t size() const
		{ return keys.size(); }

		std::string const& key(size_t i) const
		{ return keys[i]; }

		std::string const& value(size_t i) const
		{ return values[i]; }

		bool has_key(std::string const& _k) const
		{ return std::find(keys.begin(), keys.end(), _k) != keys.end() ; }

		// get a (string) value from a key, with optional default (empty)
		std::string const& get(std::string const& _k, std::string const& _missing=std::string()) const
		{
			size_t idx = index(_k);
			if (idx == npos)
				return _missing;
			return values[idx];
		}

		// return the value of key k, defaulting to _missing
		template<typename T>
		T get(std::string const& _k, T const& _missing) const
		{
			T ret = _missing;
			size_t idx = index(_k);
			if (idx != npos)
				std::stringstream(values[idx]) >> ret;
			return ret;
		}

		// Add a key-value pair.
		template<typename T>
		void add(std::string const& _k, T const& _v)
		{
			(void)index(_k, true);

			create_kval(_k, _v);
		}

		// Add a key-value pair where the value is an array of values
		template<typename ... T>
		void add_multi(std::string const& _k, T const& ... values)
		{
			(void)index(_k, true);

			std::stringstream ss;
			ss.precision(16);
			ss << "{ ";
			append_values(ss, size_t(0), values...);
			create_kval(_k, ss.str());
		}

		// get the value of a key as an array of strings (splitting the original
		// value at commas
		std::vector<std::string> get_values(std::string const& _k) const
		{
			std::string const& v = this->get(_k);
			std::stringstream ss(v);
			std::vector<std::string> ret;
			std::string cur;
			while (getline(ss, cur, ',')) {
				ret.push_back(trim(cur));
			}
			return ret;
		}

		// Convert a value into a tuple of arbitrary types
		template<typename ... T>
		std::tuple<T...> get_tuple(std::string const& _k) const
		{
			std::vector<std::string> strings = get_values(_k);
			std::tuple<T...> ret;

			string_to_tuple<0>(strings, ret);

			return ret;
		}
	};

	// The ENVI::Output() template class, encapsulating writing to an ENVI file.
	// Samples on-disk will be a serialization of OutputDataType (which must be
	// one of the data types symbolized in DataTypeEnum
	template<typename OutputDataType, typename StreamType = std::ofstream>
	class Output
	{
		Metadata meta;
		const std::string description;
		const size_t lines, samples, pixels;
		std::vector<std::string> channels;
		StreamType data;
		StreamType hdr;
		// Did we open data and hdr ourselves?
		bool need_closing;

		// Write out channel data, of type InputDataType.
		// The generic version does sample-by-sample conversion from
		// InputDataType to OutputDataType
		template<typename InputDataType>
		void write_channel_data(InputDataType const *ptr, size_t count)
		{
			for (size_t p = 0; p < count; ++p) {
				OutputDataType sample = ptr[p];
				data.write((const char*)&sample, sizeof(sample));
			}
		}

		// Specialization of write_channel_data when no conversion is needed
		void write_channel_data(OutputDataType const *ptr, size_t count)
		{
			data.write((const char*)ptr, count*sizeof(*ptr));
		}

		// Write out a whole channel, from data stored at ptr
		template<typename InputDataType>
		void write_channel(InputDataType const *ptr)
		{
			write_channel_data(ptr, pixels);
		}

		// Write out a whole channel, from data stored at ptr, assuming
		// that consecutive lines are at stride elements of each other.
		// (For example, because we are only storing a subset of the data,
		// or because lines of in-memory data were allocated with a larger
		// pitch than needed for alignment reasons or whatever.)
		template<typename InputDataType>
		void write_strided_channel(InputDataType const *ptr, size_t stride)
		{
			for (size_t l = 0; l < lines; ++l) {
				InputDataType const *line = ptr + l*stride;
				write_channel_data(line, samples);
			}
		}

		// Write out a whole channel, from data provided by a function
		// (or functor) that takes the current row, col as argument
		// and returns the value
		template<typename Func, typename ...Args>
		void write_channel_function(Func&& func, Args&& ... args)
		{
			for (size_t l = 0; l < lines; ++l) {
				for (size_t c = 0; c < samples; ++c) {
					OutputDataType sample = std::bind(func, args..., l, c)();
					data.write((const char*)&sample, sizeof(sample));
				}
			}
		}

		// Write channel names in the header: one per line if there's
		// more than one, space-wrapped if there's only one
		void write_channel_names()
		{
			size_t num = channels.size();
			hdr << (num > 1 ? "\n" : " ");
			for (size_t c = 0; c < num - 1; ++c)
				hdr << channels[c] << ",\n";
			hdr << channels.back();
			hdr << (num > 1 ? "\n" : " ");
		}

		// Write out the whole header
		void write_header()
		{
			hdr << "ENVI\n";
			hdr << "description = { " << description << " }\n";
			hdr << "samples = " << samples << "\n";
			hdr << "lines = " << lines << "\n";
			hdr << "bands = " << channels.size() << "\n";
			hdr << "data type = " << TypeCode<OutputDataType>() << "\n";
			hdr << "interleave = bsq\n"; // TODO user choice
			hdr << "header offset = 0\n" ;
			hdr << "byte order = "
				<< endianness() // TODO user choice:
				<< "\n" ;
			hdr << "band names = {" ;
			write_channel_names();
			hdr << "}\n";

			for (size_t i = 0; i < meta.size(); ++i)
			{
				hdr << meta.key(i) << " = " << meta.value(i) << "\n";
			}
		}

		void prepare_writing()
		{
			data.exceptions(std::ios::failbit | std::ios::badbit);
			hdr.exceptions(std::ios::failbit | std::ios::badbit);
		}

		void flush()
		{
			data.flush();
			write_header();
			hdr.flush();
		}

		// TODO enable only if StreamType has 'close'
		void close()
		{
			data.close();
			hdr.close();
		}
	public:
		// Create output, with given data and header streams,
		// description, and number of lines and samples
		Output( StreamType&& data_stream,
			StreamType&& hdr_stream,
			std::string const& _desc,
			size_t _lines, size_t _samples) :
			description(_desc),
			lines(_lines),
			samples(_samples),
			pixels(lines*samples),
			channels(),
			data(data_stream),
			hdr(hdr_stream),
			need_closing(false)
		{
			prepare_writing();
		}

		// Create output, with given name, description
		// and number of lines and samples (columns).
		// The header file will have the extension replaced by
		// '.hdr' (or hdr appended) automatically
		Output(std::string const& fname,
			std::string const& _desc,
			size_t _lines, size_t _samples) :
			description(_desc),
			lines(_lines),
			samples(_samples),
			pixels(lines*samples),
			channels(),
			data(StreamType(fname)),
			hdr(StreamType(hdr_name(fname))),
			need_closing(true)
		{
			prepare_writing();
		}

		~Output()
		{
			// Finalize the files on closure, but only if they are valid
			// otherwise we might get an exception thrown during stack
			// unwinding
			if (data && hdr) try {
				flush();
				if (need_closing)
					close();
			} catch (std::exception &e) {
				// nothing we can do in a destructor anyway
			}
		}

		// Add a channel
		template<typename InputDataType>
		size_t add_channel(std::string const& ch_name,
			InputDataType const* ptr)
		{
			write_channel(ptr);
			channels.push_back(ch_name);
			return channels.size() - 1;
		}

		template<typename InputDataType>
		size_t add_channel(std::string const& ch_name,
			std::vector<InputDataType> const& vec)
		{
			if (vec.size() != lines*samples)
				throw std::runtime_error("wrong number of pixels in channel " + ch_name);
			return add_channel(ch_name, &vec.front());
		}

		// Add a channel from a linearized array with the given
		// stride (in elements), starting from the given row and column
		template<typename InputDataType>
		size_t add_channel_rect(std::string const& ch_name,
			InputDataType const* ptr, size_t stride,
			size_t row=0, size_t col=0)
		{
			if (stride < samples + col)
				throw std::runtime_error("data stride too small in channel " + ch_name);
			write_strided_channel(ptr + row*stride + col, stride);
			channels.push_back(ch_name);
			return channels.size() - 1;
		}

		template<typename InputDataType>
		size_t add_channel_rect(std::string const& ch_name,
			std::vector<InputDataType> const& vec, size_t stride,
			size_t row=0, size_t col=0)
		{
			if ( (row+lines)*stride < vec.size())
				throw std::runtime_error("vector too small for channel " + ch_name);
			return add_channel_rect(ch_name, &vec.front(), stride, row, col);
		}

		// Add a channel defined by applying a function to (row, column) pairs
		template<typename Func, typename ...Args>
		size_t add_channel_func(std::string const& ch_name, Func&& func, Args&& ... args)
		{
			write_channel_function(func, args...);
			channels.push_back(ch_name);
			return channels.size() - 1;
		}

		// Add a single-valued meta key
		template<typename T>
		void add_meta(std::string const& key, T const& value)
		{
			meta.add(key, value);
		}

		// Add a multi-valued meta key
		template<typename ...T>
		void add_meta(std::string const& key, T const& ... value)
		{
			meta.add_multi(key, value...);
		}
	};

	// The Input file class needs to be defined after defining the CodeType maps,
	// since they need to know CodeType has a type member which is a type.
	// Forward-declare it here
	template<typename StreamType = std::ifstream>
	class BasicInput;

	typedef BasicInput<std::ifstream> Input;

public:

	// Open an ENVI file for writing, specifying
	// the number of rows (lines) and columns (samples). If the file already exists,
	// it will be overwritten.
	template<typename OutputDataType>
	static std::shared_ptr<Output<OutputDataType>>
	create(std::string const& output_fname, std::string const& desc,
		size_t lines, size_t samples)
	{
		return std::shared_ptr<Output<OutputDataType>>(
			new Output<OutputDataType>(output_fname, desc, lines, samples));
	}

	// Comfort method to write a single-channel file
	template<typename OutputDataType>
	static void
	dump(std::string const& output_fname, std::string const& desc,
		size_t lines, size_t samples, OutputDataType const *data)
	{
		auto f = create<OutputDataType>(output_fname, desc, lines, samples);
		f->add_channel(desc, data);
	}

	// Comfort method to write a single-channel file
	template<typename OutputDataType>
	static void
	dump(std::string const& output_fname, std::string const& desc,
		size_t lines, size_t samples, std::vector<OutputDataType> const& data)
	{
		auto f = create<OutputDataType>(output_fname, desc, lines, samples);
		f->add_channel(desc, data);
	}

	// Open an ENVI file for reading
	static inline std::shared_ptr<Input>
	ropen(std::string const& input_fname);

	// Method to load a single channel from a file. This will be
	// only declared here, as its definition depends on the ENVI::Input
	// definition
	template<typename OutputDataType, typename ChannelSpec>
	static void
	undump(std::string const& input_fname, ChannelSpec const& channel,
		size_t &lines, size_t &samples, std::vector<OutputDataType>& data);

	// Comfort method to load a single-channel file. Same as undump() with channel = 0,
	// but throws if there is more than one channel
	template<typename OutputDataType>
	static void
	undump(std::string const& input_fname,
		size_t &lines, size_t &samples, std::vector<OutputDataType>& data);

};

#define DEFINE_DATA_TYPE(typ, key) \
	template<> struct \
	ENVI::TypeCode<typ> \
	{ \
		constexpr operator DataTypeEnum(void) const \
		{ return ENVI::DataTypeEnum::key; } \
	}; \
	template<> struct \
	ENVI::CodeType<ENVI::DataTypeEnum::key> \
	{ \
		typedef typ type; \
	}

DEFINE_DATA_TYPE(int8_t, CHAR);
DEFINE_DATA_TYPE(int16_t, INT16);
DEFINE_DATA_TYPE(int32_t, INT32);
DEFINE_DATA_TYPE(float, FP32);
DEFINE_DATA_TYPE(double, FP64);
#if CXXENVI_COMPLEX
DEFINE_DATA_TYPE(std::complex<float>, FP32C);
DEFINE_DATA_TYPE(std::complex<double>, FP64C);
#endif
DEFINE_DATA_TYPE(uint16_t, UINT16);
DEFINE_DATA_TYPE(uint32_t, UINT32);
DEFINE_DATA_TYPE(int64_t, INT64);
DEFINE_DATA_TYPE(uint64_t, UINT64);


// Class to manage input from 'arbitrary' istreams
// TODO expose metadata
// TODO allow reading of all channels at once
template<typename StreamType>
class ENVI::BasicInput
{
protected:
	Metadata meta;
	std::string description;
	size_t lines, samples, pixels;
	size_t data_offset;
	DataTypeEnum input_data_type;
	std::vector<std::string> channels;
	StreamType data;
	StreamType hdr;
	bool need_closing;

	// We assume that each key = value is in a separate line,
	// except for array/string values, that begin with '{' and end
	// with '}' (followed by a newline). So if an input contains a
	// { we keep reading lines until we find the closing }
	// TODO return information on whether it was in {} or not
	inline void read_keyval(std::string &key, std::string &val)
	{
		std::string keyval;
		std::string line;

		key.clear();
		val.clear();
		while (line.empty())
		{
			std::getline(hdr, line);
			if (!hdr)
				return; // nothing else to read
		}

		keyval = line;
		size_t open = keyval.find('{');
		size_t close = keyval.find('}');

		// if there was no closing '}', keep reading until one is
		// found 
		if ( open != keyval.npos && close == keyval.npos)
		{
			while (close == keyval.npos)
			{
				std::getline(hdr, line);
				keyval += line;
				if (hdr.fail())
					throw std::runtime_error("missing '}'");
				close = keyval.find('}');
			}
		}

		// the key is up to the =, excluding it
		size_t eq = keyval.find('=');
		if (eq == keyval.npos || eq > open)
			throw std::runtime_error("missing '='");

		// for the key, just trim whitespace
		key = keyval.substr(0, eq);
		trim(key);

		// for the val, get between open and close if they are npos,
		// or everything past the equal sign otherwise
		size_t val_start = (open != keyval.npos ? open : eq)+1;
		size_t val_len = (open != keyval.npos ? close - open-1 : keyval.npos);
		val = keyval.substr(val_start, val_len);
		trim(val);
	}

	void process_keyval(std::string const& key, std::string const& val)
	{
		if (key == "description") {
			description = val;
		} else if (key == "samples") {
			samples = atol(val.c_str());
		} else if (key == "lines") {
			lines = atol(val.c_str());
		} else if (key == "bands") {
			size_t nbands = atol(val.c_str());
			if (channels.size() > 0 && nbands != channels.size())
				throw std::runtime_error("inconsistent bands and band names");
			channels.reserve(nbands);
		} else if (key == "data type") {
			long type = atol(val.c_str());
			if (!valid_type(type))
				throw std::invalid_argument("unknown ENVI type '" + val);
			input_data_type = (DataTypeEnum)type;
		} else if (key == "interleave") {
			if (val != "bsq")
				throw std::invalid_argument("interleave '" + val + "' not supported");
		} else if (key == "header offset") {
			data_offset = atol(val.c_str());
		} else if (key == "byte order") {
			size_t bo = atol(val.c_str());
			if (bo)
				throw std::invalid_argument("unsupported endianness");
		} else if (key == "band names") {
			// if we read a 'bands', we expect as many names as there were bands,
			// so read the capacity we reserved when bands was read, if any
			const size_t expected = channels.capacity();
			if (channels.size() > 0)
				throw std::invalid_argument("'band names' seen twice");

			// comma separated list
			size_t prev = 0, found = 0;
			while ((found = val.find(',', prev)) != val.npos) {
				std::string sec = val.substr(prev, found-prev);
				channels.push_back(trim(sec));
				prev = found+1;
			}
			// push the part after the last comma, if not empty
			std::string rem = val.substr(prev, val.npos);
			trim(rem);
			if (!rem.empty())
				channels.push_back(rem);

			if (expected && channels.size() != expected)
				throw std::runtime_error("inconsistent band names and bands");
		} else {
			meta.add(key, val);
		}
	}

	void read_header()
	{
		std::string line;
		std::getline(hdr, line);
		if (line != "ENVI")
			throw std::runtime_error("missing 'ENVI' in header");

		std::string key, val;

		do {
			read_keyval(key, val);
			if (key.empty())
				break;

#if CXXENVI_DEBUG
			std::clog << "KEY: '" << key << "', VAL: '" << val << "'" << std::endl;
#endif

			process_keyval(key, val);
		} while (hdr);

		pixels = lines*samples;
		// TODO other consistency checks etc

	}

	void prepare_reading()
	{
		data.exceptions(std::ios::badbit);
		hdr.exceptions(std::ios::badbit);
		read_header();
	}


	// TODO enable only if StreamType has 'close'
	void close()
	{
		data.close();
		hdr.close();
	}

	// Loader template class. Since we need runtime switching based off the
	// type specified in the header, this will recursively call itself until
	// matching the required data type
	template<DataTypeEnum input_type = CHAR>
	struct Loader
	{
		typedef typename CodeType<input_type>::type InputType;

		template<typename OutputType>
		static inline void
		undump(size_t count, std::istream &data, OutputType *o_data)
		{
			for (size_t px = 0; px < count; ++px) {
				InputType val;
				data.read(reinterpret_cast<char*>(&val), sizeof(InputType));
				o_data[px] = val;
			}
		}

		// Specialization for matching type
		static inline void
		undump(size_t count, std::istream &data, InputType *o_data)
		{
			data.read(reinterpret_cast<char*>(o_data), count*sizeof(InputType));
		}


		template<typename OutputType>
		static inline void
		undump(size_t count, std::istream &data, std::vector<OutputType>& o_data)
		{
			for (size_t px = 0; px < count; ++px) {
				InputType val;
				data.read(reinterpret_cast<char*>(&val), sizeof(InputType));
				o_data[px] = val;
			}
		}

		// Specialization for matching type
		static inline void
		undump(size_t count, std::istream &data, std::vector<InputType>& o_data)
		{
			data.read(reinterpret_cast<char*>(&(o_data[0])), count*sizeof(InputType));
		}


		template<typename OutputType>
		static inline void
		prep_load(Input *in, size_t chnum, OutputType *o_data)
		{
			size_t raw_offset = in->data_offset + chnum*in->pixels*sizeof(InputType);
			in->data.seekg(raw_offset);

			undump(in->pixels, in->data, o_data);
		}

		template<typename OutputType>
		static inline void
		prep_load(Input *in, size_t chnum, std::vector<OutputType>& o_data)
		{
			size_t raw_offset = in->data_offset + chnum*in->pixels*sizeof(InputType);
			in->data.seekg(raw_offset);

			undump(in->pixels, in->data, o_data);
		}


		template<typename OutputType>
		static inline void
		load(DataTypeEnum req, Input *in, size_t chnum, std::vector<OutputType>& o_data)
		{
			if (req == input_type)
				return prep_load(in, chnum, o_data);
			// this shouldn't happen:
			if (input_type == UINT64)
				throw std::invalid_argument("invalid input type");
			Loader<next_type(input_type)>::load(req, in, chnum, o_data);
		}

		template<typename OutputType>
		static inline void
		load(DataTypeEnum req, Input *in, size_t chnum, OutputType *o_data)
		{
			if (req == input_type)
				return prep_load(in, chnum, o_data);
			// this shouldn't happen:
			if (input_type == UINT64)
				throw std::invalid_argument("invalid input type");
			Loader<next_type(input_type)>::load(req, in, chnum, o_data);
		}
	};

	template<DataTypeEnum input_type>
	friend struct Loader;

public:
	BasicInput(StreamType&& _data, StreamType&& _hdr) :
		description(),
		lines(0),
		samples(0),
		pixels(0),
		data_offset(0),
		channels(),
		data(_data),
		hdr(_hdr),
		need_closing(false)
	{
		prepare_reading();
	}

	BasicInput(std::string const& fname) :
		description(),
		lines(0),
		samples(0),
		pixels(0),
		data_offset(0),
		channels(),
		data(StreamType(fname)),
		hdr(StreamType(hdr_name(fname)))
	{
		if (!hdr.good()) {
			hdr = StreamType(fname + ".hdr");
		}
		need_closing = true;

		prepare_reading();
	}

	std::pair<size_t, size_t> extent() const
	{ return std::make_pair(lines, samples); }

	size_t num_channels() const
	{ return channels.size(); }

	std::vector<std::string> const& channel_names() const
	{ return channels; }

	std::string const& get_meta(std::string const& key) const
	{ return meta.get(key); }

	template<typename T>
	T get_meta(std::string const& key, T const& missing) const
	{ return meta.get(key, missing); }

	template<typename ...T>
	std::vector<std::string> get_meta_values(std::string const& key) const
	{ return meta.get_values(key); }

	template<typename ...T>
	std::tuple<T...> get_meta_tuple(std::string const& key) const
	{ return meta.get_tuple<T...>(key); }

	bool has_meta(std::string const& key) const
	{ return meta.has_key(key); }

	template<typename ...T>
	void get_meta_tuple(std::string const& key, T&... args) const
	{ std::tie(args...) = meta.get_tuple<T...>(key); }

	// Load channel number chnum
	template<typename OutputType>
	void get_channel(size_t chnum, size_t &o_lines, size_t &o_samples,
		std::vector<OutputType>& o_data)
	{
		if (chnum >= channels.size())
			throw std::invalid_argument("channel number too high");

		o_lines = lines;
		o_samples = samples;
		o_data.resize(pixels);

		Loader<>::load(input_data_type, this, chnum, o_data);
	}

	template<typename OutputType>
	void get_channel(std::string const& channel, size_t &o_lines, size_t &o_samples,
		std::vector<OutputType>& o_data)
	{
		auto channel_idx(
			std::find(channels.cbegin(), channels.cend(), channel)
			);

		if (channel_idx == channels.cend())
			throw std::runtime_error("channel " + channel + " not found");

		const size_t chnum = channel_idx - channels.cbegin();

		get_channel(chnum, o_lines, o_samples, o_data);
	}

	template<typename OutputType>
	void get_channel(size_t chnum, OutputType *o_data)
	{
		if (chnum >= channels.size())
			throw std::invalid_argument("channel number too high");

		Loader<>::load(input_data_type, this, chnum, o_data);
	}

	~BasicInput()
	{
		if (need_closing)
			close();
	}
};

template<>
inline void ENVI::string_extract<decltype(std::ignore)>(std::string const& /* str */, decltype(std::ignore)&)
{}

std::shared_ptr<ENVI::Input> ENVI::ropen(std::string const& input_fname)
{
	return std::shared_ptr<Input>(new Input(input_fname));
}

template<typename OutputDataType, typename ChannelSpec>
void ENVI::undump(std::string const& input_fname, ChannelSpec const& channel,
	size_t &lines, size_t &samples, std::vector<OutputDataType>& data)
{
	Input loader(input_fname);

	loader.get_channel(channel, lines, samples, data);
}

template<typename OutputDataType>
void ENVI::undump(std::string const& input_fname,
	size_t &lines, size_t &samples, std::vector<OutputDataType>& data)
{
	Input loader(input_fname);

	if (loader.num_channels() > 1)
		throw std::runtime_error("file has multiple channel, cannot do a simple undump");

	loader.get_channel(0, lines, samples, data);
}

#endif
