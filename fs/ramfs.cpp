
#include <string.h>

struct File {
    const char* path;
    const char* content;
};

static File files[4];
static int fileCount = 0;

void initFS() {
    writeFile("home/welcome.txt", "Welcome to SCos Notepad!");
}

const char* readFile(const char* path) {
    for (int i = 0; i < fileCount; ++i) {
        if (strcmp(files[i].path, path) == 0)
            return files[i].content;
    }
    return "(file not found)";
}

bool writeFile(const char* path, const char* data) {
    for (int i = 0; i < fileCount; ++i) {
        if (strcmp(files[i].path, path) == 0) {
            files[i].content = data;
            return true;
        }
    }
    if (fileCount < 4) {
        files[fileCount].path = path;
        files[fileCount].content = data;
        fileCount++;
        return true;
    }
    return false;
}
