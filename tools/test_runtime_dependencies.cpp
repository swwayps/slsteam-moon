// TDD regression test for the single runtime-dependency diagnostic.

#include "runtime_dependencies.hpp"

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}

	bool createExecutable(const std::string& directory, const char* name,
	                      mode_t mode = 0700)
	{
		const std::string path = directory + "/" + name;
		const int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, mode);
		if (fd < 0) return false;
		close(fd);
		return chmod(path.c_str(), mode) == 0;
	}
}

int main()
{
	check(RuntimeDependencies::missingTools(true, true).empty(),
	      "no missing tools produces no detail");
	check(RuntimeDependencies::missingTools(false, true) == "timeout",
	      "missing timeout is identified");
	check(RuntimeDependencies::missingTools(true, false) == "python3",
	      "missing python3 is identified");
	check(RuntimeDependencies::missingTools(false, false) == "timeout, python3",
	      "all missing tools share one diagnostic");

	char rootTemplate[] = "/tmp/slsteam-runtime-deps-XXXXXX";
	char* rootPath = mkdtemp(rootTemplate);
	if (rootPath == nullptr)
	{
		check(false, "temporary PATH fixture can be created");
	}
	else
	{
		// The probe names what run-steamless.sh runs; unzip and gzip are not
		// used by anything the library starts.
		const std::string root(rootPath);
		check(RuntimeDependencies::missingToolsOnPath(root.c_str())
		          == "timeout, python3",
		      "PATH probe reports all missing helpers");
		check(createExecutable(root, "unzip") && createExecutable(root, "gzip")
		          && createExecutable(root, "timeout"),
		      "PATH fixture creates unzip, gzip and timeout");
		check(RuntimeDependencies::missingToolsOnPath(root.c_str()) == "python3",
		      "unzip and gzip do not stand in for python3");
		check(createExecutable(root, "python3", 0600),
		      "PATH fixture creates a non-executable python3");
		check(RuntimeDependencies::missingToolsOnPath(root.c_str()) == "python3",
		      "PATH probe recognizes only executable helpers");
		check(chmod((root + "/python3").c_str(), 0700) == 0,
		      "PATH fixture makes python3 executable");
		check(RuntimeDependencies::missingToolsOnPath(root.c_str()).empty(),
		      "PATH probe accepts all executable helpers");
		for (const char* name : {"unzip", "gzip", "timeout", "python3"})
		{
			unlink((root + "/" + name).c_str());
		}
		rmdir(root.c_str());
	}

	if (failures != 0)
	{
		std::fprintf(stderr, "test_runtime_dependencies: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("runtime dependency tests passed");
	return 0;
}
