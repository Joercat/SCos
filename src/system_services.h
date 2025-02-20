class SystemServices {
public:
    class NetworkService {
        void connect();
        void disconnect();
        bool isConnected();
    };

    class AudioService {
        void playSound(const std::string& file);
        void setVolume(float level);
    };

    class RenderService {
        GLFWwindow* window;
        VkInstance instance;
        VkPhysicalDevice physicalDevice;
        VkDevice device;

    public:
        void initVulkan();
        void createSwapChain();
        void render();
        void cleanup();
    };

private:
    NetworkService network;
    AudioService audio;
    RenderService renderer;
};
