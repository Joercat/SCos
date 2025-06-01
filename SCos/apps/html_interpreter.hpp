
#pragma once
#include <stdint.h>

#define MAX_HTML_SIZE 4096
#define MAX_CSS_RULES 100
#define MAX_JS_FUNCTIONS 50
#define MAX_DOM_ELEMENTS 200

struct HTMLElement {
    char tag[32];
    char id[64];
    char class_name[64];
    char content[256];
    char attributes[512];
    int x, y, width, height;
    uint8_t color;
    bool visible;
    int parent_id;
    int children[10];
    int child_count;
};

struct CSSRule {
    char selector[64];
    char property[32];
    char value[64];
    bool active;
};

struct JSFunction {
    char name[64];
    char body[512];
    bool active;
};

class HTMLInterpreter {
public:
    static void init();
    static bool parseHTML(const char* html_content);
    static bool parseCSS(const char* css_content);
    static bool parseJS(const char* js_content);
    static void renderPage(int window_id);
    static void handleClick(int x, int y);
    static void executeJS(const char* function_name);
    static void reset();
    static HTMLElement* getElementById(const char* id);
    static void updateElementContent(const char* id, const char* new_content);
    static void toggleElementVisibility(const char* id);

private:
    static int parseHTMLElement(const char* element_text, int parent_id);
    static void applyCSSRules();
    static void renderElement(HTMLElement* element, int window_id);
    static void calculateLayout();
    static uint8_t parseColor(const char* color_name);
    static void parseAttributes(const char* attrs, HTMLElement* element);
    static void parseInlineStyle(const char* style, HTMLElement* element);
    static void applyCSSProperty(HTMLElement* element, const char* property, const char* value);
    static bool matchesSelector(HTMLElement* element, const char* selector);
    static void extractTagName(const char* element_text, char* tag_name);
    static bool isVoidElement(const char* tag_name);
    static void setDefaultElementProperties(HTMLElement* element);
    static int getElementSpacing(HTMLElement* element);
    static void flashScreen();
    static int findElementById(const char* id);
    static int findElementByTag(const char* tag);
};
