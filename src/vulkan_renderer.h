class VulkanRenderer {
private:
    VkSwapchainKHR swapChain;
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    
public:
    void initialize();
    void createRenderPass();
    void createGraphicsPipeline();
    void drawFrame();
};
