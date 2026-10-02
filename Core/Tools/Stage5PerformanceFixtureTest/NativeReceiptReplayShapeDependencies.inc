// Test-only external string/filesystem/process-boundary collaborators.
// Include inside the existing producer fixture, after its AsciiString class.
// The actual public route, wildcard resolver, worker loop, and intent remain native.
class UnicodeString
{
public:
	void translate(const AsciiString &text)
	{
		value.clear();
		for (const unsigned char *next = reinterpret_cast<const unsigned char *>(text.str()); *next; ++next)
		{
			native_receipt_caller_fence::Require(*next < 128,
				"controlled worker command input uses the declared ASCII test lane");
			value.push_back(static_cast<wchar_t>(*next));
		}
	}
	void format(const wchar_t *formatText, ...)
	{
		wchar_t buffer[4096] = {};
		va_list arguments;
		va_start(arguments, formatText);
		const int length = vswprintf_s(buffer, ARRAY_SIZE(buffer), formatText, arguments);
		va_end(arguments);
		native_receipt_caller_fence::Require(length >= 0,
			"actual worker command format fits the bounded external string collaborator");
		value.assign(buffer, static_cast<size_t>(length));
	}
	const wchar_t *str() const { return value.c_str(); }
private:
	std::wstring value;
};

typedef std::vector<AsciiString> FilenameList;
struct LocalFileSystem
{
	LocalFileSystem() : wildcardEnumerationCalls(0) {}
	void getFileListInDirectory(const char *, const char *, const AsciiString &,
		FilenameList &, Bool)
	{
		// A native explicit trace must reject wildcard input before the public
		// route reaches this filesystem enumeration boundary.
		++wildcardEnumerationCalls;
	}
	unsigned wildcardEnumerationCalls;
} localFileSystem;
LocalFileSystem *TheLocalFileSystem = &localFileSystem;

// Only the external OS-process boundary is controlled. No CreateProcess,
// child handle, native exit observation, receipt, or child identity is produced.
// The source-exact worker loop builds, starts, polls, and consumes this result.
class WorkerProcess
{
public:
	WorkerProcess() : started(false), done(false) {}
	bool startProcess(UnicodeString command)
	{
		Check(!started, "actual worker loop starts each external process boundary once");
		started = true;
		++outcome.processStartCalls;
		outcome.workerCommand = command.str();
		return true;
	}
	void update() { if (started) done = true; }
	bool isRunning() const { return started && !done; }
	bool isDone() const { return done; }
	DWORD getExitCode() const
	{
		Check(done, "actual worker loop reads the external result only after polling completion");
		return 0;
	}
	AsciiString getStdOutput() const
	{
		return AsciiString("controlled process boundary; no native child executed\n");
	}
private:
	bool started, done;
};
