class WindowManager {
private:
    std::vector<Window*> windows;
    Desktop desktop;
    Taskbar taskbar;
    
public:
    void createWindow(const std::string& title, int x, int y, int width, int height);
    void render();
    void handleInput(InputEvent event);
};
