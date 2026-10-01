// DsHttpDownload.cpp -- httpc file download, driven by the game loop.
#ifdef CTR_PLATFORM

#include "3ds/qr/DsHttpDownload.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "platform/Log.h"

namespace
{
// One receive slice per poll(): 16 KiB lands in ~a frame at the console's
// real-world Wi-Fi rate, and the timeout bounds what a stalled server can
// cost the menu per frame.
constexpr std::uint32_t kReceiveChunkBytes = 16 * 1024;
constexpr std::uint64_t kSliceTimeoutNs = 100 * 1000 * 1000LL;

// The whole point of a QR download is skins, packs and mods; anything bigger
// than this is not one, and streaming it to SD anyway would just waste the
// card's space behind a bar that never seems to end.
constexpr std::uint32_t kMaxDownloadBytes = 96u * 1024u * 1024u;

// The system httpc verifies HTTPS against the roots it was issued; these are
// the public ones in that set. A site whose chain the console does not know
// fails with a clear error rather than a silent hang -- use an http:// URL
// for such hosts.
const SSLC_DefaultRootCert kPublicRootCerts[] = {
	SSLC_DefaultRootCert_CyberTrust,
	SSLC_DefaultRootCert_AddTrust_External_CA,
	SSLC_DefaultRootCert_COMODO,
	SSLC_DefaultRootCert_USERTrust,
	SSLC_DefaultRootCert_DigiCert_EV,
};

std::string describeHttpFailure(Result rc)
{
	return "the server or the connection failed (" + std::to_string(rc) + ")";
}
} // namespace

DsHttpDownload::~DsHttpDownload()
{
	cancel();
}

bool DsHttpDownload::begin(const std::string &url, const std::string &destPathValue,
                           std::string &outError)
{
	outError.clear();
	cancel();

	if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
	{
		outError = "The code does not contain a http(s) web address";
		return false;
	}

	Result rc = httpcInit(0);
	if (R_FAILED(rc))
	{
		outError = "The HTTP service is unavailable (http:C)";
		MC_LOG_WARN("3ds", "qr: httpcInit failed %08lX\n", static_cast<unsigned long>(rc));
		return false;
	}
	httpcStarted = true;

	rc = httpcOpenContext(&context, HTTPC_METHOD_GET, url.c_str(), 0);
	if (R_FAILED(rc))
	{
		outError = "The address could not be opened";
		MC_LOG_WARN("3ds", "qr: httpcOpenContext failed %08lX\n", static_cast<unsigned long>(rc));
		cancel();
		return false;
	}
	contextOpen = true;

	if (url.rfind("https://", 0) == 0)
	{
		for (SSLC_DefaultRootCert cert : kPublicRootCerts)
			httpcAddDefaultCert(&context, cert);
	}
	httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_DISABLED);
	httpcAddRequestHeaderField(&context, "User-Agent", "OptiCraft-Heritage/3DS");
	httpcAddRequestHeaderField(&context, "Connection", "close");

	rc = httpcBeginRequest(&context);
	if (R_FAILED(rc))
	{
		outError = "The request could not be sent";
		cancel();
		return false;
	}

	file = std::fopen(destPathValue.c_str(), "wb");
	if (file == nullptr)
	{
		outError = "The destination file could not be created";
		cancel();
		return false;
	}
	destPath = destPathValue;
	received = 0;
	total = 0;
	receiveChunk.assign(kReceiveChunkBytes, 0);
	phase = Phase::Headers;
	return true;
}

DsHttpDownload::Status DsHttpDownload::poll(std::string &outError)
{
	outError.clear();
	if (phase == Phase::Idle)
		return Status::Idle;
	if (phase == Phase::Headers)
	{
		std::uint32_t statusCode = 0;
		const Result rc = httpcGetResponseStatusCodeTimeout(&context, &statusCode, kSliceTimeoutNs);
		if (rc == HTTPC_RESULTCODE_TIMEDOUT)
			return Status::Busy; // headers not in yet; try again next frame
		if (R_FAILED(rc))
		{
			fail(describeHttpFailure(rc), outError);
			return Status::Failed;
		}
		if (statusCode != 200)
		{
			fail("The server answered " + std::to_string(statusCode), outError);
			return Status::Failed;
		}
		std::uint32_t downloaded = 0;
		std::uint32_t contentSize = 0;
		httpcGetDownloadSizeState(&context, &downloaded, &contentSize);
		total = contentSize;
		if (total > kMaxDownloadBytes)
		{
			fail("The file is larger than what this console downloads", outError);
			return Status::Failed;
		}
		phase = Phase::Receiving;
		return Status::Busy;
	}

	// Receiving: ask for at most one chunk, or what remains when the size
	// is known -- never over-request, so rc==0 really means "that much
	// arrived" and the end is detected by the byte counter, not by parsing
	// httpc's return codes.
	std::uint32_t want = kReceiveChunkBytes;
	if (total != 0 && total - received < want)
		want = total - received;

	const std::uint32_t before = received;
	const Result rc = httpcReceiveDataTimeout(&context, receiveChunk.data(), want, kSliceTimeoutNs);

	std::uint32_t downloaded = 0;
	std::uint32_t contentSize = 0;
	if (R_FAILED(httpcGetDownloadSizeState(&context, &downloaded, &contentSize)))
		downloaded = before; // keep the previous counter on a broken query
	if (contentSize != 0)
		total = contentSize;
	received = downloaded;

	const std::uint32_t delta = downloaded >= before ? downloaded - before : 0;
	if (delta != 0 && std::fwrite(receiveChunk.data(), 1, delta, file) != delta)
	{
		fail("The SD card rejected the data", outError);
		return Status::Failed;
	}
	if (total != 0 && received > kMaxDownloadBytes)
	{
		fail("The file is larger than what this console downloads", outError);
		return Status::Failed;
	}

	if (R_SUCCEEDED(rc))
	{
		if (total != 0)
		{
			if (received >= total)
			{
				std::fflush(file);
				std::fclose(file);
				file = nullptr;
				finishContext();
				phase = Phase::Idle;
				// The file is complete and stays: a later cancel() (the
				// screen closing, the destructor) must not treat it as a
				// partial and delete it.
				destPath.clear();
				MC_LOG_INFO("3ds", "qr: download complete, %u bytes\n", received);
				return Status::Done;
			}
			return Status::Busy;
		}
		// No Content-Length (chunked transfer): a receive that succeeded
		// without delivering anything is the stream's end. The next frame's
		// poll lands here once; the first such read is the end because the
		// previous read already consumed everything.
		if (delta == 0)
		{
			std::fflush(file);
			std::fclose(file);
			file = nullptr;
			finishContext();
			phase = Phase::Idle;
			destPath.clear();
			MC_LOG_INFO("3ds", "qr: download complete (no length), %u bytes\n", received);
			return Status::Done;
		}
		return Status::Busy;
	}
	if (rc == HTTPC_RESULTCODE_DOWNLOADPENDING)
		return Status::Busy;

	fail(describeHttpFailure(rc), outError);
	return Status::Failed;
}

void DsHttpDownload::cancel()
{
	if (file != nullptr)
	{
		std::fclose(file);
		file = nullptr;
	}
	finishContext();
	if (!destPath.empty())
		removePartialFile();
	destPath.clear();
	phase = Phase::Idle;
}

void DsHttpDownload::finishContext()
{
	if (contextOpen)
	{
		// httpcCloseContext hangs on an unfinished transfer; cancelling
		// first is the documented way out of an aborted download.
		httpcCancelConnection(&context);
		httpcCloseContext(&context);
		contextOpen = false;
	}
	if (httpcStarted)
	{
		httpcExit();
		httpcStarted = false;
	}
}

void DsHttpDownload::removePartialFile()
{
	std::remove(destPath.c_str());
}

void DsHttpDownload::fail(const std::string &reason, std::string &outError)
{
	MC_LOG_WARN("3ds", "qr: download failed: %s\n", reason.c_str());
	outError = reason;
	if (file != nullptr)
	{
		std::fclose(file);
		file = nullptr;
	}
	finishContext();
	removePartialFile();
	destPath.clear();
	phase = Phase::Idle;
}

#endif // CTR_PLATFORM
