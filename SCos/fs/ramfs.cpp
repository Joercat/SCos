
// Custom string comparison for freestanding environment
static int custom_strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

struct File {
    const char* path;
    const char* content;
};

static File files[4];
static int fileCount = 0;

bool initFS() {
    writeFile("home/welcome.txt", "Welcome to SCos Notepad!");
    return true;
}

const char* readFile(const char* path) {
    for (int i = 0; i < fileCount; ++i) {
        if (custom_strcmp(files[i].path, path) == 0)
            return files[i].content;
    }
    return "(file not found)";
}

bool writeFile(const char* path, const char* data) {
    for (int i = 0; i < fileCount; ++i) {
        if (custom_strcmp(files[i].path, path) == 0) {
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
