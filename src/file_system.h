class FileSystem {
private:
    struct FileNode {
        std::string name;
        std::vector<uint8_t> data;
        std::vector<FileNode*> children;
    };
    
    FileNode* root;
    
public:
    void createFile(const std::string& path);
    void deleteFile(const std::string& path);
    std::vector<uint8_t> readFile(const std::string& path);
    void writeFile(const std::string& path, const std::vector<uint8_t>& data);
};
