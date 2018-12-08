#include "pdb_reader.hpp"
#include <unordered_set>

/**
 * Microsoft program debug database file
 *
 * File is a multi-stream file with various different data streams. Some streams are located at a fixed index:
 *  - Stream 0: MSF root directory copy
 *  - Stream 1: PDB headers and list of named streams
 *  - Stream 2: Type info (TPI stream)
 *  - Stream 3: Debug info (DBI stream)
 *  - Stream 4: Build info, UDT source file + line info and some function identifiers (TPI header followed by CodeView records)
 */

#pragma pack(1)

namespace blink
{
	namespace
	{
		struct pdb_header
		{
			uint32_t version;
			uint32_t time_date_stamp;
			uint32_t age;
			guid guid;
			uint32_t names_map_offset;
		};
		struct pdb_names_header
		{
			uint32_t signature;
			uint32_t version;
			uint32_t names_map_offset;
		};
		struct pdb_tpi_header
		{
			uint32_t version;
			uint32_t header_size;
			uint32_t min_index;
			uint32_t max_index;
			uint32_t content_size;
		};
		struct pdb_dbi_header
		{
			uint32_t signature;
			uint32_t version;
			uint32_t age;
			uint16_t global_symbol_info_stream;
			uint16_t pdb_dll_version;
			uint16_t public_symbol_info_stream;
			uint16_t pdb_dll_build_major;
			uint16_t symbol_record_stream;
			uint16_t pdb_dll_build_minor;
			uint32_t module_info_size;
			uint32_t section_contribution_size;
			uint32_t section_map_size;
			uint32_t file_info_size;
			uint32_t ts_map_size;
			uint32_t mfc_index;
			uint32_t debug_header_size;
			uint32_t ec_info_size;
			uint16_t flags;
			uint16_t machine;
			uint32_t reserved;
		};
		struct pdb_dbi_module_info
		{
			uint32_t opened;
			struct {
				uint16_t index;
				uint16_t padding1;
				uint32_t offset;
				uint32_t size;
				uint32_t flags;
				uint16_t module;
				uint16_t padding2;
				uint32_t data_crc;
				uint32_t relocation_crc;
			} section;
			uint16_t flags;
			uint16_t stream;
			uint32_t symbol_bytes;
			uint32_t old_lines_bytes;
			uint32_t lines_bytes;
			uint16_t num_files;
			uint16_t padding;
			uint32_t offsets;
			uint32_t num_source;
			uint32_t num_compiler;
		};
		struct pdb_dbi_debug_header
		{
			uint16_t fpo;
			uint16_t exception;
			uint16_t fixup;
			uint16_t omap_to_src;
			uint16_t omap_from_src;
			uint16_t section_header;
			uint16_t token_rid_map;
			uint16_t xdata;
			uint16_t pdata;
			uint16_t new_fpo;
			uint16_t section_header_orig;
		};
		struct pdb_section_header
		{
			char name[8];
			uint32_t size;
			uint32_t virtual_address;
			uint32_t data_size;
			uint32_t raw_data_rva;
			uint32_t relocation_table_rva;
			uint32_t line_numbers_rva;
			uint16_t num_relocations;
			uint16_t num_line_numbers;
			uint32_t flags;
		};

		unsigned int read_num(msf_stream_reader &reader)
		{
			const auto value = reader.read<uint16_t>();

			if (value >= 0x8000)
			{
				switch (value)
				{
					case 0x8000:
						return reader.read<uint8_t>();
					case 0x8001:
					case 0x8002:
						return reader.read<uint16_t>();
					case 0x8003:
					case 0x8004:
						return reader.read<uint16_t>();
					default:
						__debugbreak();
						return 0;
				}
			}
			else
			{
				return value;
			}
		}
		std::string encode_num(size_t num)
		{
			if (num == 0)
			{
				return "A@";
			}
			else if (num <= 10)
			{
				return { static_cast<char>('0' + num - 1) };
			}
			else
			{
				std::string result;

				for (; num != 0; num >>= 4)
				{
					result = static_cast<char>('A' + (num & 0xF)) + result;
				}

				return result + '@';
			}
		}
		size_t decode_num(std::string &data)
		{
			size_t result = 0;

			if (data[0] >= '0' && data[0] <= '9')
			{
				result = data[0] - '0' + 1;

				data.erase(0, 1);
			}
			else
			{
				size_t i = 0;
				const auto end = data.find_first_of('@');

				for (auto c : data.substr(0, end))
				{
					result += (c - 'A') << i;
					i += 4;
				}

				data.erase(0, end + 1);
			}

			return result;
		}
	}

	pdb_reader::pdb_reader(const std::string &path) : msf_reader(path)
	{
		if (!msf_reader::is_valid() || msf_reader::stream_count() <= 4)
			return;

		// Read PDB info stream
		msf_stream_reader pdbstream(msf_reader::stream(1));

		if (pdbstream.size() == 0)
			return;

		const auto pdbheader = pdbstream.read<pdb_header>();

		_version = pdbheader.version;
		_timestamp = pdbheader.time_date_stamp;
		_guid = pdbheader.guid;

		// Read stream names from string hash map
		pdbstream.seek(sizeof(pdb_header) + pdbheader.names_map_offset);
		const auto count = pdbstream.read<uint32_t>();
		const auto hash_table_size = pdbstream.read<uint32_t>();

		_named_streams.reserve(count);

		const auto num_bitset_present = pdbstream.read<uint32_t>();
		std::vector<uint32_t> bitset_present(num_bitset_present);
		pdbstream.read(bitset_present.data(), num_bitset_present * sizeof(uint32_t));
		const auto num_bitset_deleted = pdbstream.read<uint32_t>();
		pdbstream.skip(num_bitset_deleted * sizeof(uint32_t));

		for (unsigned int i = 0; i < hash_table_size; i++)
		{
			if ((bitset_present[i / 32] & (1 << (i % 32))) == 0)
				continue;

			const auto name_offset = pdbstream.read<uint32_t>();
			const auto stream_index = pdbstream.read<uint32_t>();

			const auto oldpos = pdbstream.tell();
			pdbstream.seek(sizeof(pdb_header) + name_offset);
			const auto name = pdbstream.read<std::string>();
			pdbstream.seek(oldpos);

			_named_streams.insert({ name, stream_index });
		}

		_is_valid = stream_count() > 4;
	}

	std::vector<type> pdb_reader::types()
	{
		// Read type info (TPI stream)
		msf_stream_reader stream(msf_reader::stream(2));
		const auto header = stream.read<pdb_tpi_header>();

		if (header.header_size + header.content_size != stream.size())
			return {};

		// Skip any additional bytes that were appended to the header
		stream.seek(header.header_size);

		// Create type table
		std::vector<type> types(header.max_index, { 0 });
		// { index, base_type_index, mangled_name, size, is_builtin, is_const, is_volatile, is_unaligned, is_array, is_pointer, is_function, is_forward_reference }
		types[0x0003] = { "X", 0, 0x0000, true, false, false, false, false, false, false, false }; // T_VOID [void]
		types[0x0103] = { "PAX", 4, 0x0003, true, false, false, false, false, true, false, false }; // T_PVOID [void *]
		types[0x0403] = { "PAX", 4, 0x0003, true, false, false, false, false, true, false, false }; // T_32PVOID [void * __ptr32]
		types[0x0603] = { "PEAX", 8, 0x0003, true, false, false, false, false, true, false, false }; // T_64PVOID [void * __ptr64]
		types[0x0070] = { "D", 1, 0x0000, true, false, false, false, false, false, false, false }; // T_RCHAR [char]
		types[0x0470] = { "PAD", 4, 0x0070, true, false, false, false, false, true, false, false }; // T_32PRCHAR [char * __ptr32]
		types[0x0670] = { "PEAD", 8, 0x0070, true, false, false, false, false, true, false, false }; // T_64PRCHAR [char * __ptr64]
		types[0x0071] = { "_W", 2, 0x0000, true, false, false, false, false, false, false, false }; // T_WCHAR [wchar_t]
		types[0x0471] = { "PA_W", 4, 0x0071, true, false, false, false, false, true, false, false }; // T_32PWCHAR [wchar_t * __ptr32]
		types[0x0671] = { "PEA_W", 8, 0x0071, true, false, false, false, false, true, false, false }; // T_64PWCHAR [wchar_t * __ptr64]
		types[0x007A] = { "_S", 2, 0x0000, true, false, false, false, false, false, false, false }; // T_CHAR16 [char16_t]
		types[0x047A] = { "PA_S", 4, 0x007A, true, false, false, false, false, true, false, false }; // T_32PCHAR16 [char16_t * __ptr32]
		types[0x067A] = { "PEA_S", 8, 0x007A, true, false, false, false, false, true, false, false }; // T_64PCHAR16 [char16_t * __ptr64]
		types[0x007B] = { "_U", 4, 0x0000, true, false, false, false, false, false, false, false }; // T_CHAR32 [char32_t]
		types[0x047B] = { "PA_U", 4, 0x007B, true, false, false, false, false, true, false, false }; // T_32PCHAR32 [char32_t * __ptr32]
		types[0x067B] = { "PEA_U", 8, 0x007B, true, false, false, false, false, true, false, false }; // T_64PCHAR32 [char32_t * __ptr64]
		types[0x0010] = { "C", 1, 0x0000, true, false, false, false, false, false, false, false }; // T_CHAR [signed char]
		types[0x0410] = { "PAC", 4, 0x0010, true, false, false, false, false, true, false, false }; // T_32PCHAR [signed char * __ptr32]
		types[0x0610] = { "PEAC", 8, 0x0010, true, false, false, false, false, true, false, false }; // T_64PCHAR [signed char * __ptr64]
		types[0x0020] = { "E", 1, 0x0000, true, false, false, false, false, false, false, false }; // T_UCHAR [unsigned char]
		types[0x0420] = { "PAE", 4, 0x0020, true, false, false, false, false, true, false, false }; // T_32PUCHAR [unsigned char * __ptr32]
		types[0x0620] = { "PEAE", 8, 0x0020, true, false, false, false, false, true, false, false }; // T_64PUCHAR [unsigned char * __ptr64]
		types[0x0011] = { "F", 2, 0x0000, true, false, false, false, false, false, false, false }; // T_SHORT [short]
		types[0x0411] = { "PAF", 4, 0x0011, true, false, false, false, false, true, false, false }; // T_32PSHORT [short * __ptr32]
		types[0x0611] = { "PEAF", 8, 0x0011, true, false, false, false, false, true, false, false }; // T_64PSHORT [short * __ptr64]
		types[0x0021] = { "G", 2, 0x0000, true, false, false, false, false, false, false, false }; // T_USHORT [unsigned short]
		types[0x0421] = { "PAG", 4, 0x0021, true, false, false, false, false, true, false, false }; // T_32PUSHORT [unsigned short * __ptr32]
		types[0x0621] = { "PEAG", 8, 0x0021, true, false, false, false, false, true, false, false }; // T_64PUSHORT [unsigned short * __ptr64]
		types[0x0074] = { "H", 4, 0x0000, true, false, false, false, false, false, false, false }; // T_INT4 [int]
		types[0x0474] = { "PAH", 4, 0x0074, true, false, false, false, false, true, false, false }; // T_32PINT4 [int * __ptr32]
		types[0x0674] = { "PEAH", 8, 0x0074, true, false, false, false, false, true, false, false }; // T_64PINT4 [int * __ptr64]
		types[0x0075] = { "I", 4, 0x0000, true, false, false, false, false, false, false, false }; // T_UINT4 [unsigned int]
		types[0x0475] = { "PAI", 4, 0x0075, true, false, false, false, false, true, false, false }; // T_32PUINT4 [unsigned int * __ptr32]
		types[0x0675] = { "PEAI", 8, 0x0075, true, false, false, false, false, true, false, false }; // T_64PUINT4 [unsigned int * __ptr64]
		types[0x0012] = { "J", 4, 0x0000, true, false, false, false, false, false, false, false }; // T_LONG [long]
		types[0x0412] = { "PAJ", 4, 0x0012, true, false, false, false, false, true, false, false }; // T_32PLONG [long * __ptr32]
		types[0x0612] = { "PEAJ", 8, 0x0012, true, false, false, false, false, true, false, false }; // T_64PLONG [long * __ptr64]
		types[0x0022] = { "K", 4, 0x0000, true, false, false, false, false, false, false, false }; // T_ULONG [unsigned long]
		types[0x0422] = { "PAK", 4, 0x0022, true, false, false, false, false, true, false, false }; // T_32PULONG [unsigned long * __ptr32]
		types[0x0622] = { "PEAK", 8, 0x0022, true, false, false, false, false, true, false, false }; // T_64PULONG [unsigned long * __ptr64]
		types[0x0013] = { "_J", 8, 0x0000, true, false, false, false, false, false, false, false }; // T_QUAD [__int64]
		types[0x0413] = { "PA_J", 4, 0x0013, true, false, false, false, false, true, false, false }; // T_32PQUAD [__int64 * __ptr32]
		types[0x0613] = { "PEA_J", 8, 0x0013, true, false, false, false, false, true, false, false }; // T_64PQUAD [__inte64 * __ptr64]
		types[0x0023] = { "_K", 8, 0x0000, true, false, false, false, false, false, false, false }; // T_UQUAD [unsigned __int64]
		types[0x0423] = { "PA_K", 4, 0x0023, true, false, false, false, false, true, false, false }; // T_32PUQUAD [unsigned __int64 * __ptr32]
		types[0x0623] = { "PEA_K", 8, 0x0023, true, false, false, false, false, true, false, false }; // T_64PUQUAD [unsigned __int64 * __ptr64]
		types[0x0076] = { "_J", 8, 0x0000, true, false, false, false, false, false, false, false }; // T_INT8 [__int64]
		types[0x0476] = { "PA_J", 4, 0x0076, true, false, false, false, false, true, false, false }; // T_32PINT8 [__int64 * __ptr32]
		types[0x0676] = { "PEA_J", 8, 0x0076, true, false, false, false, false, true, false, false }; // T_64PINT8 [__int64 * __ptr64]
		types[0x0077] = { "_K", 8, 0x0000, true, false, false, false, false, false, false, false }; // T_UINT8 [unsigned __int64]
		types[0x0477] = { "PA_K", 4, 0x0077, true, false, false, false, false, true, false, false }; // T_32PUINT8 [unsigned __int64 * __ptr32]
		types[0x0677] = { "PEA_K", 8, 0x0077, true, false, false, false, false, true, false, false }; // T_64PUINT8 [unsigned __int64 * __ptr64]
		types[0x0040] = { "M", 4, 0x0000, true, false, false, false, false, false, false, false }; // T_REAL32 [float]
		types[0x0440] = { "PAM", 4, 0x0040, true, false, false, false, false, true, false, false }; // T_32PREAL32 [float * __ptr32]
		types[0x0640] = { "PEAM", 8, 0x0040, true, false, false, false, false, true, false, false }; // T_64PREAL32 [float * __ptr64]
		types[0x0041] = { "N", 8, 0x0000, true, false, false, false, false, false, false, false }; // T_REAL64 [double]
		types[0x0441] = { "PAN", 4, 0x0041, true, false, false, false, false, true, false, false }; // T_32PREAL64 [double * __ptr32]
		types[0x0641] = { "PEAN", 8, 0x0041, true, false, false, false, false, true, false, false }; // T_64PREAL64 [double * __ptr64]
		types[0x0030] = { "_N", 8, 0x0000, true, false, false, false, false, false, false, false }; // T_BOOL08 [bool]
		types[0x0430] = { "PA_N", 4, 0x0030, true, false, false, false, false, true, false, false }; // T_32PBOOL08 [bool * __ptr32]
		types[0x0630] = { "PEA_N", 8, 0x0030, true, false, false, false, false, true, false, false }; // T_64PBOOL08 [bool * __ptr64]
		types[0x0008] = { "J", 4, 0x0000, true, false, false, false, false, false, false, false }; // T_HRESULT [HRESULT]
		types[0x0408] = { "PAJ", 4, 0x0008, true, false, false, false, false, true, false, false }; // T_32PHRESULT [HRESULT * __ptr32]
		types[0x0608] = { "PEAJ", 8, 0x0008, true, false, false, false, false, true, false, false }; // T_64PHRESULT [HRESULT * __ptr64]

		// A list of type records in CodeView format
		for (auto current_index = header.min_index; current_index < header.max_index; current_index++)
		{
			// Each records starts with 2 bytes containing the size of the record after this element
			const auto size = stream.read<uint16_t>();
			// Next 2 bytes contain an enumeration depicting the type and format of the following data
			const auto tag = stream.read<uint16_t>();
			// The next record is found by adding the current record size to the position of the previous size element
			const auto next_record_offset = (stream.tell() - sizeof(uint16_t)) + size;

			switch (tag)
			{
				case 0x1001: // LF_MODIFIER
				{
					struct leaf_data
					{
						uint32_t base_type_index;
						uint16_t is_const : 1;
						uint16_t is_volatile : 1;
						uint16_t is_unaligned : 1;
						uint16_t reserved : 13;
					};

					const auto info = stream.read<leaf_data>();
					const auto basetype = types[info.base_type_index];

					types[current_index] = { basetype.mangled_name, basetype.size, info.base_type_index, false, (info.is_const != 0), (info.is_volatile != 0), (info.is_unaligned != 0), false, false, false, (basetype.is_forward_reference != 0) };
					break;
				}
				case 0x1002: // LF_POINTER
				{
					struct leaf_data
					{
						uint32_t base_type_index;
						uint32_t ptr_type : 5;
						uint32_t ptr_mode : 3;
						uint32_t is_flat32 : 1;
						uint32_t is_volatile : 1;
						uint32_t is_const : 1;
						uint32_t is_unaligned : 1;
						uint32_t is_restrict : 1;
						uint32_t size : 6;
						uint32_t is_mocom : 1;
						uint32_t is_member_lref : 1;
						uint32_t is_member_rref : 1;
						uint32_t reserved : 10;
					};

					const auto info = stream.read<leaf_data>();
					auto basetype = types[info.base_type_index];
					std::string mangled_name;

					switch (info.ptr_mode)
					{
						case 0: // normal pointer
						case 3: // method pointer
							mangled_name += info.is_const && info.is_volatile ? 'S' : info.is_const ? 'Q' : info.is_volatile ? 'R' : 'P';
							break;
						case 1: // l-value reference
							mangled_name += info.is_volatile ? 'B' : 'A';
							break;
						case 4: // r-value reference
							mangled_name += info.is_volatile ? "$$R" : "$$Q";
							break;
					}

					if (basetype.is_function)
					{
						if (info.ptr_mode == 3)
						{
							mangled_name += '8' + basetype.mangled_name.erase(basetype.mangled_name.rfind("@@") + 2, 1);
						}
						else
						{
							mangled_name += '6' + basetype.mangled_name.erase(0, 2);
						}
					}
					else
					{
						if (info.size == 8)
						{
							mangled_name += 'E';
						}

						if (info.is_restrict)
						{
							mangled_name += 'I';
						}

						if (basetype.is_unaligned)
						{
							mangled_name += 'F';
						}

						mangled_name += basetype.is_const && basetype.is_volatile ? 'D' : basetype.is_const ? 'B' : basetype.is_volatile ? 'C' : 'A';
						mangled_name += basetype.mangled_name;
					}

					types[current_index] = { std::move(mangled_name), info.size, info.base_type_index, false, (info.is_const != 0), (info.is_volatile != 0), (info.is_unaligned != 0), false, true, false, false };
					break;
				}
				case 0x1008: // LF_PROCEDURE
				{
					struct leaf_data
					{
						uint32_t return_type_index;
						uint8_t calling_convention;
						uint8_t is_return_type_udt : 1;
						uint8_t reserved : 7;
						uint16_t param_count;
						uint32_t arg_list_type_index;
					};

					const auto info = stream.read<leaf_data>();
					const auto return_type = types[info.return_type_index];
					const auto arg_list_type = types[info.arg_list_type_index];
					std::string mangled_name;

					mangled_name += 'Y';

					switch (info.calling_convention)
					{
						case 0x00: // __cdecl
							mangled_name += 'A';
							break;
						case 0x02: // __pascal
							mangled_name += 'C';
							break;
						case 0x04: // __fastcall
							mangled_name += 'I';
							break;
						case 0x07: // __stdcall
							mangled_name += 'G';
							break;
						case 0x0B: // __thiscall
							mangled_name += 'E';
							break;
						case 0x18: // __vectorcall
							mangled_name += 'Q';
							break;
					}

					if (!return_type.is_builtin && !return_type.is_pointer)
					{
						mangled_name += '?';

						if (return_type.is_unaligned)
						{
							mangled_name += 'F';
						}

						mangled_name += return_type.is_const && return_type.is_volatile ? 'D' : return_type.is_const ? 'B' : return_type.is_volatile ? 'C' : 'A';
					}

					mangled_name += return_type.mangled_name;
					mangled_name += arg_list_type.mangled_name;
					mangled_name += 'Z';

					types[current_index] = { std::move(mangled_name), 0, 0, false, false, false, false, false, false, true, false };
					break;
				}
				case 0x1009: // LF_MFUNCTION
				{
					struct leaf_data
					{
						uint32_t return_type_index;
						uint32_t class_type_index;
						uint32_t this_ptr_type_index;
						uint8_t calling_convention;
						uint8_t is_return_type_udt : 1;
						uint8_t is_ctor : 1;
						uint8_t is_ctor_vbase : 1;
						uint8_t reserved : 5;
						uint16_t param_count;
						uint32_t arg_list_type_index;
						int32_t this_adjust_offset;
					};

					const auto info = stream.read<leaf_data>();
					const auto return_type = types[info.return_type_index];
					const auto arg_list_type = types[info.arg_list_type_index];
					std::string mangled_name;

					if (info.this_ptr_type_index != 0)
					{
						const auto this_ptr_type = types[info.this_ptr_type_index];
						const auto this_ptr_basetype = types[this_ptr_type.base_type_index];

						if (this_ptr_type.size == 8)
						{
							mangled_name += 'E';
						}

						mangled_name += this_ptr_basetype.is_const && this_ptr_basetype.is_volatile ? 'D' : this_ptr_basetype.is_const ? 'B' : this_ptr_basetype.is_volatile ? 'C' : 'A';
					}

					switch (info.calling_convention)
					{
						case 0x00: // __cdecl
							mangled_name += 'A';
							break;
						case 0x02: // __pascal
							mangled_name += 'C';
							break;
						case 0x04: // __fastcall
							mangled_name += 'I';
							break;
						case 0x07: // __stdcall
							mangled_name += 'G';
							break;
						case 0x0B: // __thiscall
							mangled_name += 'E';
							break;
						case 0x18: // __vectorcall
							mangled_name += 'Q';
							break;
					}

					if (!return_type.is_builtin && !return_type.is_pointer)
					{
						mangled_name += '?';

						if (return_type.is_unaligned)
						{
							mangled_name += 'F';
						}

						mangled_name += return_type.is_const && return_type.is_volatile ? 'D' : return_type.is_const ? 'B' : return_type.is_volatile ? 'C' : 'A';
					}

					if (info.is_ctor || info.is_ctor_vbase)
					{
						mangled_name += '@';
					}
					else
					{
						mangled_name += return_type.mangled_name;
					}

					mangled_name += arg_list_type.mangled_name;
					mangled_name += 'Z';

					types[current_index] = { std::move(mangled_name), 0, info.class_type_index, false, false, false, false, false, false, true, false };
					break;
				}
				case 0x1201: // LF_ARGLIST
				{
					const auto count = stream.read<uint32_t>();
					bool is_variadic = false;
					size_t argument_size = 0;
					std::string mangled_name;

					for (unsigned int i = 0; i < count; i++)
					{
						const auto type_index = stream.read<uint32_t>();

						if (type_index == 0)
						{
							is_variadic = true;
							break;
						}
						else
						{
							const auto type = types[type_index];

							mangled_name += type.mangled_name;
							argument_size += type.size;
						}
					}

					if (count == 0)
					{
						mangled_name += 'X';
					}
					else if (is_variadic)
					{
						mangled_name += 'Z';
					}
					else
					{
						mangled_name += '@';
					}

					types[current_index] = { std::move(mangled_name), argument_size, 0, false, false, false, false, false, false, false, false };
					break;
				}
				case 0x1503: // LF_ARRAY
				{
					struct leaf_type
					{
						uint32_t element_type_index;
						uint32_t indexing_type_index;
					};

					const auto info = stream.read<leaf_type>();
					const auto actual_size = read_num(stream);
					auto element_type = types[info.element_type_index];
					std::string mangled_name;

					mangled_name += 'Y';

					if (element_type.is_array)
					{
						element_type.mangled_name.erase(0, 1);
						mangled_name += encode_num(decode_num(element_type.mangled_name) + 1);
					}
					else
					{
						mangled_name += encode_num(1);
					}

					if (element_type.is_forward_reference)
					{
						mangled_name += encode_num(0);
					}
					else
					{
						mangled_name += encode_num(actual_size / element_type.size);
					}

					mangled_name += element_type.mangled_name;

					types[current_index] = { std::move(mangled_name), actual_size, info.element_type_index, false, false, false, false, true, false, false, (element_type.is_forward_reference != 0) };
					break;
				}
				case 0x1504: // LF_CLASS
				case 0x1505: // LF_STRUCTURE
				case 0x1506: // LF_UNION
				{
					struct leaf_data
					{
						uint16_t element_count;
						uint16_t is_packed : 1;
						uint16_t has_constructors : 1;
						uint16_t has_overloaded_operators : 1;
						uint16_t is_nested : 1;
						uint16_t has_nested_types : 1;
						uint16_t has_overloaded_assignment : 1;
						uint16_t has_overloaded_casting : 1;
						uint16_t is_forward_reference : 1;
						uint16_t is_scoped : 1;
						uint16_t has_unique_name : 1;
						uint16_t is_sealed : 1;
						uint16_t hfa_kind : 2; // HFA = homogeneous floating-point aggregate
						uint16_t is_intrinsic : 1;
						uint16_t mocom_udt_kind : 2; // MoCOM UDT = Modern COM user-defined type
						uint32_t field_descriptor_type_index;
					};

					const auto info = stream.read<leaf_data>();

					if (tag != 0x1506)
					{
						stream.skip(8); // skip derived and virtual function table shape type index
					}

					const auto actual_size = read_num(stream);
					std::string mangled_name = stream.read<std::string>();

					if (info.has_unique_name)
					{
						mangled_name.swap(stream.read<std::string>().erase(0, 3));
					}
					else
					{
						mangled_name.insert(0, 1, static_cast<char>('T' + (0x1506 - tag))) += "@@";
					}

					types[current_index] = { std::move(mangled_name), actual_size, 0, false, false, false, false, false, false, false, (info.is_forward_reference != 0) };
					break;
				}
				case 0x1507: // LF_ENUM
				{
					struct leaf_data
					{
						uint16_t element_count;
						uint16_t is_packed : 1;
						uint16_t has_constructors : 1;
						uint16_t has_overloaded_operators : 1;
						uint16_t is_nested : 1;
						uint16_t has_nested_types : 1;
						uint16_t has_overloaded_assignment : 1;
						uint16_t has_overloaded_casting : 1;
						uint16_t is_forward_reference : 1;
						uint16_t is_scoped : 1;
						uint16_t has_unique_name : 1;
						uint16_t is_sealed : 1;
						uint16_t hfa_kind : 2;
						uint16_t is_intrinsic : 1;
						uint16_t mocom_udt_kind : 2;
						uint32_t base_type_index;
						uint32_t field_descriptor_type_index;
					};

					const auto info = stream.read<leaf_data>();
					std::string mangled_name = stream.read<std::string>();

					if (info.has_unique_name)
					{
						mangled_name.swap(stream.read<std::string>().erase(0, 3));
					}
					else
					{
						mangled_name.insert(0, "W4", 2) += "@@";
					}

					types[current_index] = { std::move(mangled_name), types[info.base_type_index].size, info.base_type_index, false, false, false, false, false, false, false, (info.is_forward_reference != 0) };
					break;
				}
			}

			stream.seek(next_record_offset);

			// Each element is aligned to 4-byte boundary
			stream.align(4);
		}

		return types;
	}
	std::unordered_map<std::string, ptrdiff_t> pdb_reader::symbols()
	{
		// Read debug info (DBI stream)
		msf_stream_reader stream(msf_reader::stream(3));
		const auto dbiheader = stream.read<pdb_dbi_header>();

		if (dbiheader.signature != 0xFFFFFFFF)
			return {};

		// Read section headers
		stream.seek(sizeof(pdb_dbi_header) + dbiheader.module_info_size + dbiheader.section_contribution_size + dbiheader.section_map_size + dbiheader.file_info_size + dbiheader.ts_map_size + dbiheader.ec_info_size);
		const auto dbgheader = stream.read<pdb_dbi_debug_header>();
		msf_stream_reader sectionstream(msf_reader::stream(dbgheader.section_header));

		std::vector<pdb_section_header> sections;
		sections.reserve(sectionstream.size() / sizeof(pdb_section_header));

		while (sectionstream.tell() < sectionstream.size())
		{
			// The section header stream is a tightly packed list of section header structures
			sections.push_back(std::move(sectionstream.read<pdb_section_header>()));
		}

		// Read symbol table
		stream = msf_reader::stream(dbiheader.symbol_record_stream);
		std::unordered_map<std::string, ptrdiff_t> symbols;

		// A list of records in CodeView format
		while (stream.tell() < stream.size())
		{
			// Each records starts with 2 bytes containing the size of the record after this element
			const auto size = stream.read<uint16_t>();
			// Next 2 bytes contain an enumeration depicting the type and format of the following data
			const auto tag = stream.read<uint16_t>();
			// The next record is found by adding the current record size to the position of the previous size element
			const auto next_record_offset = (stream.tell() - sizeof(uint16_t)) + size;

			switch (tag)
			{
				case 0x110E: // S_PUB32
				{
					struct leaf_data
					{
						uint32_t is_code : 1;
						uint32_t is_function : 1;
						uint32_t is_managed : 1;
						uint32_t is_managed_il : 1;
						uint32_t padding : 28;
						uint32_t offset;
						uint16_t segment;
					};

					const auto info = stream.read<leaf_data>();
					const auto mangled_name = stream.read<std::string>();

					if (info.segment == 0 || info.segment > sections.size())
					{
						symbols[mangled_name] = 0;
					}
					else
					{
						symbols[mangled_name] = info.offset + sections[info.segment - 1].virtual_address;
					}
					break;
				}
			}

			stream.seek(next_record_offset);

			// Each element is aligned to 4-byte boundary
			stream.align(4);
		}

		return symbols;
	}
	std::vector<std::string> pdb_reader::buildtools()
	{
		// Read build info
		msf_stream_reader stream(msf_reader::stream(4));
		const auto header = stream.read<pdb_tpi_header>();

		if (header.header_size + header.content_size != stream.size())
			return {};

		std::unordered_set<std::string> buildtools;
		std::unordered_map<unsigned int, std::string> string_table;

		// Skip any additional bytes that were appended to the header
		stream.seek(header.header_size);

		// A list of records in CodeView format
		for (auto current_index = header.min_index; current_index < header.max_index; current_index++)
		{
			// Each records starts with 2 bytes containing the size of the record after this element
			const auto size = stream.read<uint16_t>();
			// Next 2 bytes contain an enumeration depicting the type and format of the following data
			const auto tag = stream.read<uint16_t>();
			// The next record is found by adding the current record size to the position of the previous size element
			const auto next_record_offset = (stream.tell() - sizeof(uint16_t)) + size;

			switch (tag)
			{
				case 0x1603: // LF_BUILDINFO
				{
					stream.skip(6);
					const auto build_tool_id = stream.read<uint32_t>();

					buildtools.insert(string_table.at(build_tool_id));
				}
				case 0x1605: // LF_STRING_ID
				{
					stream.skip(4);
					string_table[current_index] = stream.read<std::string>();
					break;
				}
			}

			stream.seek(next_record_offset);

			// Each element is aligned to 4-byte boundary
			stream.align(4);
		}

		return std::vector<std::string>(buildtools.begin(), buildtools.end());
	}
	std::vector<std::string> pdb_reader::sourcefiles()
	{
#if 1
		// Read debug info (DBI stream)
		msf_stream_reader stream(msf_reader::stream(3));
		const auto dbiheader = stream.read<pdb_dbi_header>();

		// https://llvm.org/docs/PDB/DbiStream.html#file-info-substream
		stream.seek(sizeof(pdb_dbi_header) + dbiheader.module_info_size + dbiheader.section_contribution_size + dbiheader.section_map_size);
		const uint16_t num_modules = stream.read<uint16_t>();
		stream.skip(2 + num_modules * 2);

		uint32_t num_source_files = 0;
		for (uint16_t i = 0; i < num_modules; ++i)
			num_source_files += stream.read<uint16_t>();

		std::vector<uint32_t> file_name_offsets;
		file_name_offsets.reserve(num_source_files);
		for (uint32_t i = 0; i < num_source_files; ++i)
		{
			file_name_offsets.push_back(stream.read<uint32_t>());
		}

		auto offset = stream.tell();

		std::vector<std::string> source_files;
		source_files.reserve(num_source_files);
		for (uint32_t i = 0; i < num_source_files; ++i)
		{
			stream.seek(offset + file_name_offsets[i]);

			const std::string source_file = stream.read<std::string>();
			source_files.push_back(source_file);
		}

		return source_files;
#else
		// Read build info
		msf_stream_reader stream(msf_reader::stream(4));
		const auto header = stream.read<pdb_tpi_header>();

		if (header.header_size + header.content_size != stream.size())
			return {};

		std::unordered_set<std::string> sourcefiles;
		std::unordered_map<unsigned int, std::string> string_table;

		// Skip any additional bytes that were appended to the header
		stream.seek(header.header_size);

		// A list of records in CodeView format
		for (auto current_index = header.min_index; current_index < header.max_index; current_index++)
		{
			// Each records starts with 2 bytes containing the size of the record after this element
			const auto size = stream.read<uint16_t>();
			// Next 2 bytes contain an enumeration depicting the type and format of the following data
			const auto tag = stream.read<uint16_t>();
			// The next record is found by adding the current record size to the position of the previous size element
			const auto next_record_offset = (stream.tell() - sizeof(uint16_t)) + size;

			switch (tag)
			{
				case 0x1603: // LF_BUILDINFO
				{
					stream.skip(2);
					const auto directory_id = stream.read<uint32_t>();
					const std::string directory(string_table.at(directory_id));
					stream.skip(4);
					const auto source_file_id = stream.read<uint32_t>();
					const std::string source_file(string_table.at(source_file_id));

					if (source_file.find("f:\\dd\\vctools") == std::string::npos)
					{
						sourcefiles.insert(directory + '\\' + source_file);
					}
					break;
				}
				case 0x1605: // LF_STRING_ID
				{
					stream.skip(4);
					string_table[current_index] = stream.read<std::string>();
					break;
				}
			}

			stream.seek(next_record_offset);

			// Each element is aligned to 4-byte boundary
			stream.align(4);
		}

		return std::vector<std::string>(sourcefiles.begin(), sourcefiles.end());
#endif
	}
	std::unordered_map<unsigned int, std::string> pdb_reader::names()
	{
		msf_stream_reader stream(this->stream("/names"));

		if (!is_valid() || stream.size() == 0)
		{
			return { };
		}

		// Read names stream
		const auto header = stream.read<pdb_names_header>();

		if (header.signature != 0xEFFEEFFE || header.version != 1)
		{
			return { };
		}

		// Read string hash table
		stream.seek(sizeof(pdb_names_header) + header.names_map_offset);
		const auto size = stream.read<uint32_t>();

		std::unordered_map<unsigned int, std::string> names;
		names.reserve(size);

		for (unsigned int i = 0; i < size; i++)
		{
			const auto name_offset = stream.read<uint32_t>();

			if (name_offset == 0)
			{
				continue;
			}

			const auto oldpos = stream.tell();
			stream.seek(sizeof(pdb_names_header) + name_offset);
			const auto name = stream.read<std::string>();
			stream.seek(oldpos);

			names.insert({ i, name });
		}

		return names;
	}
}