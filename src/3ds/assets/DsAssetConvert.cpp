// DsAssetConvert.cpp -- the runtime counterpart of scripts/texturepack_flip_3ds.py.
#ifdef CTR_PLATFORM

#include "3ds/assets/DsAssetConvert.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform/Log.h"
#include "stb_image.h"
// stb's writer is compiled per TU with internal linkage, the exact shape
// SkinManager.cpp uses (STB_IMAGE_WRITE_STATIC): stbi_write_png_to_mem has
// no public prototype in this vendored header -- its definition only exists
// inside the implementation block -- so every consumer that needs it ships
// its own static copy. No ODR clash: static keeps each copy internal.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "unzip.h"
#include "zip.h"

namespace
{
const unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// The CPU colour lookup tables, in the pack namespace (entry names at the
// zip root; a pack unpacked from a pak may still carry the internal
// "assets/" prefix, which the normaliser strips). KEEP IN LOCKSTEP with
// CPU_LUT_SKIP in scripts/pak_flip_mc3ds.py -- that list is the authority
// this mirrors, and the scripts' docstrings explain each entry.
const char *const kCpuLutEntries[] = {
	"misc/grasscolor.png",
	"misc/foliagecolor.png",
	"misc/watercolor.png",
	"misc/watercolorx.png",
	"misc/pinecolor.png",
	"misc/birchcolor.png",
	"misc/swampgrasscolor.png",
	"misc/swampfoliagecolor.png",
	"misc/redstonecolor.png",
	"misc/stemcolor.png",
	"misc/myceliumparticlecolor.png",
};

std::string normaliseEntryName(const char *name)
{
	std::string lowered;
	for (const char *p = name; *p != '\0'; ++p)
	{
		char c = *p;
		if (c == '\\')
			c = '/';
		else if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
		lowered += c;
	}
	if (lowered.rfind("assets/", 0) == 0)
		lowered.erase(0, 7);
	return lowered;
}

bool isCpuLut(const char *name)
{
	const std::string normalised = normaliseEntryName(name);
	for (const char *lut : kCpuLutEntries)
	{
		if (normalised == lut)
			return true;
	}
	return false;
}

bool isPngBytes(const std::vector<unsigned char> &data)
{
	return data.size() > 8 && std::memcmp(data.data(), kPngSignature, 8) == 0;
}

// Decode -> reverse the row order -> re-encode, all in memory. stb decodes
// every PNG form to RGBA8, so the rewritten file loses palette packing but
// nothing semantic: the console uploads RGBA texels anyway, and the CPU
// tables never come through here.
bool flipPngInMemory(const std::vector<unsigned char> &in, std::vector<unsigned char> &out)
{
	int width = 0;
	int height = 0;
	int components = 0;
	stbi_uc *pixels = stbi_load_from_memory(in.data(), static_cast<int>(in.size()),
	                                         &width, &height, &components, 4);
	if (pixels == nullptr || width <= 0 || height <= 0)
	{
		if (pixels != nullptr)
			stbi_image_free(pixels);
		return false;
	}

	const int stride = width * 4;
	std::vector<stbi_uc> swapRow(static_cast<std::size_t>(stride));
	for (int y = 0; y < height / 2; ++y)
	{
		stbi_uc *top = pixels + static_cast<std::size_t>(y) * stride;
		stbi_uc *bottom = pixels + static_cast<std::size_t>(height - 1 - y) * stride;
		std::memcpy(swapRow.data(), top, static_cast<std::size_t>(stride));
		std::memcpy(top, bottom, static_cast<std::size_t>(stride));
		std::memcpy(bottom, swapRow.data(), static_cast<std::size_t>(stride));
	}

	int outLength = 0;
	unsigned char *encoded = stbi_write_png_to_mem(pixels, stride, width, height, 4, &outLength);
	stbi_image_free(pixels);
	if (encoded == nullptr || outLength <= 0)
	{
		if (encoded != nullptr)
			STBIW_FREE(encoded);
		return false;
	}
	out.assign(encoded, encoded + outLength);
	STBIW_FREE(encoded);
	return true;
}

// Read the whole current entry of an open archive into memory. The pack
// entries this walks are kilobytes-to-a-few-megabytes; a hostile "pack"
// that declares an absurd uncompressed size is refused rather than bought.
bool readCurrentEntry(unzFile archive, std::vector<unsigned char> &out, std::string &outError)
{
	unz_file_info info;
	char nameBuffer[1024];
	if (unzGetCurrentFileInfo(archive, &info, nameBuffer, sizeof(nameBuffer),
	                          nullptr, 0, nullptr, 0) != UNZ_OK)
	{
		outError = "the pack has an unreadable entry";
		return false;
	}
	if (info.uncompressed_size > 64u * 1024u * 1024u)
	{
		outError = "the pack has an oversized entry";
		return false;
	}
	if (unzOpenCurrentFile(archive) != UNZ_OK)
	{
		outError = "the pack has an unreadable entry";
		return false;
	}
	out.resize(info.uncompressed_size);
	unsigned long offset = 0;
	while (offset < out.size())
	{
		const int count = unzReadCurrentFile(archive, out.data() + offset,
		                                     static_cast<unsigned int>(out.size() - offset));
		if (count <= 0)
			break;
		offset += static_cast<unsigned long>(count);
	}
	unzCloseCurrentFile(archive);
	if (offset != out.size())
		out.resize(offset);
	return true;
}
} // namespace

namespace DsAssetConvert
{

bool convertPng(const std::string &pngPath, std::string &outError)
{
	outError.clear();

	std::FILE *file = std::fopen(pngPath.c_str(), "rb");
	if (file == nullptr)
	{
		outError = "the file could not be opened";
		return false;
	}
	std::vector<unsigned char> bytes;
	char chunk[64 * 1024];
	std::size_t readCount = 0;
	while ((readCount = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
		bytes.insert(bytes.end(), chunk, chunk + readCount);
	std::fclose(file);
	if (bytes.empty())
	{
		outError = "the file is empty";
		return false;
	}

	std::vector<unsigned char> flipped;
	if (!flipPngInMemory(bytes, flipped))
	{
		outError = "the image could not be decoded";
		return false;
	}

	file = std::fopen(pngPath.c_str(), "wb");
	if (file == nullptr)
	{
		outError = "the file could not be written";
		return false;
	}
	const std::size_t written = std::fwrite(flipped.data(), 1, flipped.size(), file);
	std::fclose(file);
	if (written != flipped.size())
	{
		outError = "the file could not be written";
		return false;
	}
	MC_LOG_INFO("3ds", "asset convert: flipped %s (%u bytes -> %u)\n",
	            pngPath.c_str(), static_cast<unsigned>(bytes.size()),
	            static_cast<unsigned>(flipped.size()));
	return true;
}

bool convertTexturePackZip(const std::string &zipPath, std::string &outError)
{
	outError.clear();

	unzFile archive = unzOpen(zipPath.c_str());
	if (archive == nullptr)
	{
		outError = "the pack is not a readable zip";
		return false;
	}

	const std::string tempPath = zipPath + ".flip3ds";
	zipFile converted = zipOpen(tempPath.c_str(), APPEND_STATUS_CREATE);
	if (converted == nullptr)
	{
		unzClose(archive);
		outError = "the converted pack could not be created";
		return false;
	}

	int flipped = 0;
	int copied = 0;
	bool failed = false;

	if (unzGoToFirstFile(archive) == UNZ_OK)
	{
		for (;;)
		{
			char nameBuffer[1024];
			unz_file_info info;
			if (unzGetCurrentFileInfo(archive, &info, nameBuffer, sizeof(nameBuffer),
			                          nullptr, 0, nullptr, 0) != UNZ_OK)
				break;

			std::vector<unsigned char> data;
			if (!readCurrentEntry(archive, data, outError))
			{
				failed = true;
				break;
			}

			// Directory entries arrive as names ending in '/' with empty
			// payloads; minizip's writer takes plain names, so carry them
			// through and let the reader rebuild the tree.
			std::vector<unsigned char> payload = std::move(data);
			if (isPngBytes(payload) && !isCpuLut(nameBuffer))
			{
				std::vector<unsigned char> flippedPng;
				if (flipPngInMemory(payload, flippedPng))
				{
					payload = std::move(flippedPng);
					++flipped;
				}
				else
				{
					// Same contract as the scripts: a PNG the flipper
					// cannot rewrite (exotic encoding, corrupt) is copied
					// verbatim with a warning rather than losing the pack.
					++copied;
					MC_LOG_WARN("3ds", "asset convert: LEFT AS-IS %s (could not decode)\n",
					            nameBuffer);
				}
			}
			else
			{
				++copied;
			}

			zip_fileinfo targetInfo{};
			targetInfo.dosDate = info.dosDate;
			if (zipOpenNewFileInZip(converted, nameBuffer, &targetInfo,
			                        nullptr, 0, nullptr, 0, nullptr,
			                        Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK ||
			    (!payload.empty() &&
			     zipWriteInFileInZip(converted, payload.data(),
			                         static_cast<unsigned int>(payload.size())) != ZIP_OK))
			{
				outError = "the converted pack could not be written";
				failed = true;
				break;
			}
			zipCloseFileInZip(converted);

			if (unzGoToNextFile(archive) != UNZ_OK)
				break;
		}
	}

	zipClose(converted, nullptr);
	unzClose(archive);

	if (failed)
	{
		std::remove(tempPath.c_str());
		return false;
	}

	// Replace the original with the converted archive. rename() over an
	// existing file is not portable; the remove-then-rename order is, and
	// the window between them sits inside one install step.
	if (std::remove(zipPath.c_str()) != 0 && std::fopen(zipPath.c_str(), "rb") != nullptr)
	{
		std::remove(tempPath.c_str());
		outError = "the original pack could not be replaced";
		return false;
	}
	if (std::rename(tempPath.c_str(), zipPath.c_str()) != 0)
	{
		std::remove(tempPath.c_str());
		outError = "the converted pack could not be moved into place";
		return false;
	}

	MC_LOG_INFO("3ds", "asset convert: pack %s converted (%d flipped, %d verbatim)\n",
	            zipPath.c_str(), flipped, copied);
	return true;
}

} // namespace DsAssetConvert

#endif // CTR_PLATFORM
