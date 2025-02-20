#include <iostream>
#include <memory>
#include "kernel.h"
#include "window_manager.h"
#include "system_services.h"

int main() {
    SCosKernel kernel;
    kernel.initialize();
    
    // Initialize core system components
    auto displayManager = std::make_unique<DisplayManager>(1024, 768);
    auto windowManager = std::make_unique<WindowManager>();
    auto taskManager = std::make_unique<TaskManager>();
    
    // Boot sequence
    kernel.startBootSequence();
    return kernel.run();
}
