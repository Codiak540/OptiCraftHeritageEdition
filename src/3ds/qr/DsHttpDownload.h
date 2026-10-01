#pragma once

// DsHttpDownload.h -- an httpc-backed file download for the "Descarga QR"
// screen (src/3ds/qr/GuiQrDownload.h).
//
// The download is driven BY THE GAME LOOP, not a worker thread: poll()
// performs one bounded slice of receive work (~up to 100 ms while data is
// streaming, one 16 KiB chunk per call) and the GUI calls it once per
// frame from updateScreen(). That shape is what makes the progress bar
// update at the menu's own frame rate and B cancel land instantly --
// httpcCancelConnection plus a context close, no thread join.
//
// httpc semantics this is built on (libctru httpc.h):
//   * httpcReceiveData returns HTTPC_RESULTCODE_DOWNLOADPENDING while the
//     body is still streaming -- the wrapper (httpcDownloadData) treats
//     rc==0 as "the requested size arrived". Each call fills the buffer it
//     was handed from its start, and httpcGetDownloadSizeState reports the
//     total delivered so far, so the delta between two reads is exactly
//     what landed in this call's buffer.
//   * httpcCloseContext hangs unless the whole content was received --
//     cancel() runs httpcCancelConnection first, which is the documented
//     way out for an aborted transfer.

#include <3ds.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class DsHttpDownload
{
public:
	enum class Status
	{
		Idle,
		Busy,    // making progress; call poll() again
		Done,    // the whole file is at the destination path
		Failed,  // outError holds the reason; the partial file is removed
	};

	DsHttpDownload() = default;
	~DsHttpDownload();

	// Begin a GET of url into destPath. False + a player-readable reason
	// when the context cannot even be opened (bad URL, httpc unavailable).
	bool begin(const std::string &url, const std::string &destPath, std::string &outError);

	// One slice of receive work. Never blocks longer than the slice budget,
	// so the menu keeps drawing between calls.
	Status poll(std::string &outError);

	// Abort an in-flight transfer (the whole point of B). Removes the
	// partial file. Safe on a finished or idle download.
	void cancel();

	std::uint32_t receivedBytes() const { return received; }
	std::uint32_t totalBytes() const { return total; }
	bool hasTotalBytes() const { return total != 0; }

private:
	enum class Phase
	{
		Idle,
		Headers,
		Receiving,
	};

	void finishContext();
	void removePartialFile();
	void fail(const std::string &reason, std::string &outError);

	Phase phase = Phase::Idle;
	bool contextOpen = false;
	bool httpcStarted = false;
	std::FILE *file = nullptr;
	std::string destPath;
	httpcContext context{};

	std::uint32_t received = 0;
	std::uint32_t total = 0;

	// One receive buffer for the download's lifetime: poll() runs every
	// frame, and a per-call 16 KiB heap churn would only age the heap.
	std::vector<unsigned char> receiveChunk;
};
