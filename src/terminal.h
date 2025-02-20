class Terminal : public Application {
private:
    CommandInterpreter interpreter;
    std::vector<std::string> commandHistory;
    
public:
    void execute(const std::string& command);
    void render();
    void handleInput(KeyEvent event);
};
