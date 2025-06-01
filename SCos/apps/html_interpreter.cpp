
#include "html_interpreter.hpp"
#include "../ui/window_manager.hpp"
#include "../include/string.h"

static HTMLElement dom_elements[MAX_DOM_ELEMENTS];
static CSSRule css_rules[MAX_CSS_RULES];
static JSFunction js_functions[MAX_JS_FUNCTIONS];
static int element_count = 0;
static int css_rule_count = 0;
static int js_function_count = 0;

// String utility functions
static int html_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void html_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static int html_strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

static bool html_strstr(const char* haystack, const char* needle) {
    if (!*needle) return true;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return true;
    }
    return false;
}

void HTMLInterpreter::init() {
    reset();
}

void HTMLInterpreter::reset() {
    element_count = 0;
    css_rule_count = 0;
    js_function_count = 0;
    
    for (int i = 0; i < MAX_DOM_ELEMENTS; i++) {
        dom_elements[i].active = false;
    }
    
    for (int i = 0; i < MAX_CSS_RULES; i++) {
        css_rules[i].active = false;
    }
    
    for (int i = 0; i < MAX_JS_FUNCTIONS; i++) {
        js_functions[i].active = false;
    }
}

bool HTMLInterpreter::parseHTML(const char* html_content) {
    // Simple HTML parser implementation
    const char* pos = html_content;
    
    while (*pos && element_count < MAX_DOM_ELEMENTS) {
        if (*pos == '<' && *(pos + 1) != '/') {
            pos++; // Skip <
            
            // Get tag name
            const char* tag_start = pos;
            while (*pos && *pos != ' ' && *pos != '>') pos++;
            
            HTMLElement* element = &dom_elements[element_count];
            
            int tag_len = pos - tag_start;
            if (tag_len > 31) tag_len = 31;
            for (int i = 0; i < tag_len; i++) {
                element->tag[i] = tag_start[i];
            }
            element->tag[tag_len] = '\0';
            
            // Parse attributes
            while (*pos && *pos != '>') {
                while (*pos && *pos == ' ') pos++; // Skip spaces
                
                if (*pos == 'i' && html_strstr(pos, "id=")) {
                    pos += 3; // Skip "id="
                    if (*pos == '"') pos++; // Skip quote
                    
                    const char* id_start = pos;
                    while (*pos && *pos != '"' && *pos != ' ' && *pos != '>') pos++;
                    
                    int id_len = pos - id_start;
                    if (id_len > 31) id_len = 31;
                    for (int i = 0; i < id_len; i++) {
                        element->id[i] = id_start[i];
                    }
                    element->id[id_len] = '\0';
                    
                    if (*pos == '"') pos++; // Skip closing quote
                }
                
                if (*pos && *pos != '>') pos++;
            }
            
            if (*pos == '>') pos++; // Skip >
            
            // Set default values
            element->x = 0;
            element->y = element_count * 2;
            element->width = 40;
            element->height = 1;
            element->color = 0x1F; // White on blue
            element->active = true;
            
            element_count++;
        } else {
            pos++;
        }
    }
    
    return true;
}

bool HTMLInterpreter::parseCSS(const char* css_content) {
    const char* pos = css_content;
    
    while (*pos && css_rule_count < MAX_CSS_RULES) {
        // Skip whitespace
        while (*pos && (*pos == ' ' || *pos == '\n' || *pos == '\t')) pos++;
        
        if (!*pos) break;
        
        // Get selector
        const char* selector_start = pos;
        while (*pos && *pos != '{' && *pos != ' ') pos++;
        
        CSSRule* rule = &css_rules[css_rule_count];
        
        int selector_len = pos - selector_start;
        if (selector_len > 31) selector_len = 31;
        for (int i = 0; i < selector_len; i++) {
            rule->selector[i] = selector_start[i];
        }
        rule->selector[selector_len] = '\0';
        
        // Skip to opening brace
        while (*pos && *pos != '{') pos++;
        if (*pos == '{') pos++;
        
        // Parse properties
        while (*pos && *pos != '}') {
            while (*pos && (*pos == ' ' || *pos == '\n' || *pos == '\t')) pos++; // Skip whitespace
            
            if (*pos == '}') break;
            
            // Get property
            const char* prop_start = pos;
            while (*pos && *pos != ':') pos++;
            
            int prop_len = pos - prop_start;
            if (prop_len > 31) prop_len = 31;
            for (int i = 0; i < prop_len; i++) {
                rule->property[i] = prop_start[i];
            }
            rule->property[prop_len] = '\0';
            
            if (*pos == ':') pos++;
            while (*pos && *pos == ' ') pos++; // Skip spaces
            
            // Get value
            const char* value_start = pos;
            while (*pos && *pos != ';' && *pos != '}') pos++;
            
            int value_len = pos - value_start;
            if (value_len > 31) value_len = 31;
            for (int i = 0; i < value_len; i++) {
                rule->value[i] = value_start[i];
            }
            rule->value[value_len] = '\0';
            
            if (*pos == ';') pos++;
            
            rule->active = true;
            css_rule_count++;
            break; // One property per rule for simplicity
        }
        
        // Skip to closing brace
        while (*pos && *pos != '}') pos++;
        if (*pos == '}') pos++;
    }
    
    return true;
}

int HTMLInterpreter::parseColor(const char* color_str) {
    if (html_strcmp(color_str, "red") == 0) return 0x4F;
    if (html_strcmp(color_str, "green") == 0) return 0x2F;
    if (html_strcmp(color_str, "blue") == 0) return 0x1F;
    if (html_strcmp(color_str, "yellow") == 0) return 0x6F;
    if (html_strcmp(color_str, "white") == 0) return 0x7F;
    return 0x1F; // Default to white on blue
}

void HTMLInterpreter::renderPage(int window_id) {
    Window* win = WindowManager::getWindow(window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int start_x = win->x + 2;
    int start_y = win->y + 4; // Start below address bar
    
    // Apply CSS rules first
    applyCSSRules();
    
    // Render elements
    for (int i = 0; i < element_count; i++) {
        HTMLElement* element = &dom_elements[i];
        if (!element->active) continue;
        
        int y = start_y + element->y;
        int x = start_x + element->x;
        
        // Render element content based on tag
        const char* content = element->tag;
        if (html_strcmp(element->tag, "h1") == 0) {
            content = "Main Title";
        } else if (html_strcmp(element->tag, "p") == 0) {
            content = "Paragraph text";
        } else if (html_strcmp(element->tag, "button") == 0) {
            content = "[Button]";
        }
        
        // Draw the content
        for (int j = 0; content[j] && j < element->width; j++) {
            if (x + j < win->x + win->width - 2 && y < win->y + win->height - 2) {
                int idx = 2 * (y * 80 + x + j);
                video[idx] = content[j];
                video[idx + 1] = element->color;
            }
        }
        
        // Update element position for click detection
        element->x = x - start_x;
        element->y = y - start_y;
    }
}

void HTMLInterpreter::reset() {
    element_count = 0;
    css_rule_count = 0;
    js_function_count = 0;
    
    for (int i = 0; i < MAX_DOM_ELEMENTS; i++) {
        dom_elements[i].tag[0] = '\0';
        dom_elements[i].visible = false;
        dom_elements[i].child_count = 0;
        dom_elements[i].parent_id = -1;
    }
    
    for (int i = 0; i < MAX_CSS_RULES; i++) {
        css_rules[i].active = false;
    }
    
    for (int i = 0; i < MAX_JS_FUNCTIONS; i++) {
        js_functions[i].active = false;
    }
}

bool HTMLInterpreter::parseHTML(const char* html_content) {
    // Simple HTML parser - looks for basic tags
    const char* pos = html_content;
    int current_parent = -1;
    
    while (*pos) {
        if (*pos == '<') {
            // Find end of tag
            const char* tag_end = pos;
            while (*tag_end && *tag_end != '>') tag_end++;
            if (!*tag_end) break;
            
            // Extract tag content
            char tag_content[256];
            int tag_len = tag_end - pos - 1;
            if (tag_len > 255) tag_len = 255;
            
            for (int i = 0; i < tag_len; i++) {
                tag_content[i] = pos[i + 1];
            }
            tag_content[tag_len] = '\0';
            
            // Skip closing tags
            if (tag_content[0] != '/') {
                parseHTMLElement(tag_content, current_parent);
            }
            
            pos = tag_end + 1;
        } else {
            pos++;
        }
    }
    
    calculateLayout();
    applyCSSRules();
    return true;
}

void HTMLInterpreter::parseHTMLElement(const char* element_text, int parent_id) {
    if (element_count >= MAX_DOM_ELEMENTS) return;
    
    HTMLElement* element = &dom_elements[element_count];
    
    // Extract tag name
    const char* space = element_text;
    while (*space && *space != ' ') space++;
    
    int tag_len = space - element_text;
    if (tag_len > 31) tag_len = 31;
    
    for (int i = 0; i < tag_len; i++) {
        element->tag[i] = element_text[i];
    }
    element->tag[tag_len] = '\0';
    
    // Parse attributes
    if (*space) {
        parseAttributes(space + 1, element);
    }
    
    // Set defaults
    element->visible = true;
    element->parent_id = parent_id;
    element->child_count = 0;
    element->color = 0x1F; // White on blue
    element->x = 0;
    element->y = element_count * 2; // Stack vertically
    element->width = 60;
    element->height = 1;
    
    // Add to parent's children
    if (parent_id >= 0 && parent_id < element_count) {
        HTMLElement* parent = &dom_elements[parent_id];
        if (parent->child_count < 10) {
            parent->children[parent->child_count++] = element_count;
        }
    }
    
    element_count++;
}

void HTMLInterpreter::parseAttributes(const char* attrs, HTMLElement* element) {
    // Simple attribute parser for id and class
    if (html_strstr(attrs, "id=")) {
        const char* id_start = attrs;
        while (*id_start && !(*id_start == 'i' && *(id_start+1) == 'd' && *(id_start+2) == '=')) {
            id_start++;
        }
        if (*id_start) {
            id_start += 4; // Skip 'id="'
            const char* id_end = id_start;
            while (*id_end && *id_end != '"') id_end++;
            
            int id_len = id_end - id_start;
            if (id_len > 63) id_len = 63;
            
            for (int i = 0; i < id_len; i++) {
                element->id[i] = id_start[i];
            }
            element->id[id_len] = '\0';
        }
    }
    
    if (html_strstr(attrs, "class=")) {
        const char* class_start = attrs;
        while (*class_start && !html_strstr(class_start, "class=")) {
            class_start++;
        }
        if (*class_start) {
            class_start += 7; // Skip 'class="'
            const char* class_end = class_start;
            while (*class_end && *class_end != '"') class_end++;
            
            int class_len = class_end - class_start;
            if (class_len > 63) class_len = 63;
            
            for (int i = 0; i < class_len; i++) {
                element->class_name[i] = class_start[i];
            }
            element->class_name[class_len] = '\0';
        }
    }
}

bool HTMLInterpreter::parseCSS(const char* css_content) {
    // Simple CSS parser
    const char* pos = css_content;
    
    while (*pos && css_rule_count < MAX_CSS_RULES) {
        // Skip whitespace
        while (*pos && (*pos == ' ' || *pos == '\n' || *pos == '\t')) pos++;
        
        if (!*pos) break;
        
        // Find selector (until {)
        const char* selector_start = pos;
        while (*pos && *pos != '{') pos++;
        if (!*pos) break;
        
        CSSRule* rule = &css_rules[css_rule_count];
        
        // Copy selector
        int sel_len = pos - selector_start;
        if (sel_len > 63) sel_len = 63;
        for (int i = 0; i < sel_len; i++) {
            rule->selector[i] = selector_start[i];
        }
        rule->selector[sel_len] = '\0';
        
        pos++; // Skip {
        
        // Find property:value pairs until }
        while (*pos && *pos != '}') {
            while (*pos && (*pos == ' ' || *pos == '\n' || *pos == '\t')) pos++;
            
            if (*pos == '}') break;
            
            // Get property
            const char* prop_start = pos;
            while (*pos && *pos != ':') pos++;
            if (!*pos) break;
            
            int prop_len = pos - prop_start;
            if (prop_len > 31) prop_len = 31;
            for (int i = 0; i < prop_len; i++) {
                rule->property[i] = prop_start[i];
            }
            rule->property[prop_len] = '\0';
            
            pos++; // Skip :
            
            while (*pos && *pos == ' ') pos++;
            
            // Get value
            const char* val_start = pos;
            while (*pos && *pos != ';' && *pos != '}') pos++;
            
            int val_len = pos - val_start;
            if (val_len > 63) val_len = 63;
            for (int i = 0; i < val_len; i++) {
                rule->value[i] = val_start[i];
            }
            rule->value[val_len] = '\0';
            
            if (*pos == ';') pos++;
            
            rule->active = true;
            css_rule_count++;
            
            if (css_rule_count >= MAX_CSS_RULES) break;
            rule = &css_rules[css_rule_count];
            html_strcpy(rule->selector, css_rules[css_rule_count-1].selector);
        }
        
        if (*pos == '}') pos++;
    }
    
    return true;
}

bool HTMLInterpreter::parseJS(const char* js_content) {
    // Simple JS function parser
    const char* pos = js_content;
    
    while (*pos && js_function_count < MAX_JS_FUNCTIONS) {
        // Look for function keyword
        if (html_strstr(pos, "function")) {
            pos += 8; // Skip "function"
            while (*pos && *pos == ' ') pos++;
            
            // Get function name
            const char* name_start = pos;
            while (*pos && *pos != '(' && *pos != ' ') pos++;
            
            JSFunction* func = &js_functions[js_function_count];
            
            int name_len = pos - name_start;
            if (name_len > 63) name_len = 63;
            for (int i = 0; i < name_len; i++) {
                func->name[i] = name_start[i];
            }
            func->name[name_len] = '\0';
            
            // Find function body (between { and })
            while (*pos && *pos != '{') pos++;
            if (!*pos) break;
            pos++; // Skip {
            
            const char* body_start = pos;
            int brace_count = 1;
            
            while (*pos && brace_count > 0) {
                if (*pos == '{') brace_count++;
                if (*pos == '}') brace_count--;
                if (brace_count > 0) pos++;
            }
            
            int body_len = pos - body_start;
            if (body_len > 511) body_len = 511;
            for (int i = 0; i < body_len; i++) {
                func->body[i] = body_start[i];
            }
            func->body[body_len] = '\0';
            func->active = true;
            js_function_count++;
        }
        
        if (*pos) pos++; // Skip closing brace
    }
    
    return true;
}
            func->body[body_len] = '\0';
            
            func->active = true;
            js_function_count++;
        } else {
            pos++;
        }
    }
    
    return true;
}

void HTMLInterpreter::applyCSSRules() {
    for (int i = 0; i < css_rule_count; i++) {
        if (!css_rules[i].active) continue;
        
        CSSRule* rule = &css_rules[i];
        
        // Apply rule to matching elements
        for (int j = 0; j < element_count; j++) {
            HTMLElement* element = &dom_elements[j];
            bool matches = false;
            
            // Check if selector matches
            if (rule->selector[0] == '#') {
                // ID selector
                matches = html_strcmp(rule->selector + 1, element->id) == 0;
            } else if (rule->selector[0] == '.') {
                // Class selector
                matches = html_strcmp(rule->selector + 1, element->class_name) == 0;
            } else {
                // Tag selector
                matches = html_strcmp(rule->selector, element->tag) == 0;
            }
            
            if (matches) {
                // Apply style
                if (html_strcmp(rule->property, "color") == 0) {
                    element->color = parseColor(rule->value);
                } else if (html_strcmp(rule->property, "width") == 0) {
                    // Simple width parsing
                    element->width = 0;
                    const char* val = rule->value;
                    while (*val >= '0' && *val <= '9') {
                        element->width = element->width * 10 + (*val - '0');
                        val++;
                    }
                }
            }
        }
    }
}

uint8_t HTMLInterpreter::parseColor(const char* color_name) {
    if (html_strcmp(color_name, "red") == 0) return 0x4F;
    if (html_strcmp(color_name, "green") == 0) return 0x2F;
    if (html_strcmp(color_name, "blue") == 0) return 0x1F;
    if (html_strcmp(color_name, "yellow") == 0) return 0x6F;
    if (html_strcmp(color_name, "white") == 0) return 0x0F;
    if (html_strcmp(color_name, "black") == 0) return 0xF0;
    return 0x1F; // Default white on blue
}

void HTMLInterpreter::calculateLayout() {
    int current_y = 0;
    
    for (int i = 0; i < element_count; i++) {
        HTMLElement* element = &dom_elements[i];
        
        if (element->parent_id == -1) { // Root level elements
            element->x = 2;
            element->y = current_y + 2;
            current_y += element->height + 1;
        }
    }
}

void HTMLInterpreter::renderPage(int window_id) {
    Window* win = WindowManager::getWindow(window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    
    for (int i = 0; i < element_count; i++) {
        if (dom_elements[i].visible) {
            renderElement(&dom_elements[i], window_id);
        }
    }
}

void HTMLInterpreter::renderElement(HTMLElement* element, int window_id) {
    Window* win = WindowManager::getWindow(window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int screen_x = win->x + element->x;
    int screen_y = win->y + element->y;
    
    // Render element content
    const char* display_text = element->content[0] ? element->content : element->tag;
    
    for (int i = 0; display_text[i] && i < element->width && screen_x + i < win->x + win->width; i++) {
        if (screen_x + i >= 0 && screen_x + i < 80 && screen_y >= 0 && screen_y < 25) {
            int idx = 2 * (screen_y * 80 + screen_x + i);
            video[idx] = display_text[i];
            video[idx + 1] = element->color;
        }
    }
}

void HTMLInterpreter::handleClick(int x, int y) {
    // Find clicked element and execute any associated JS
    for (int i = 0; i < element_count; i++) {
        HTMLElement* element = &dom_elements[i];
        if (x >= element->x && x < element->x + element->width &&
            y >= element->y && y < element->y + element->height) {
            
            // Look for onclick handler
            char onclick_func[64];
            html_strcpy(onclick_func, element->id);
            html_strcpy(onclick_func + html_strlen(onclick_func), "_click");
            
            executeJS(onclick_func);
            break;
        }
    }
}

void HTMLInterpreter::executeJS(const char* function_name) {
    for (int i = 0; i < js_function_count; i++) {
        if (js_functions[i].active && html_strcmp(js_functions[i].name, function_name) == 0) {
            // Simple JS execution - could be expanded
            // For now, just demonstrate concept
            break;
        }
    }
}

int HTMLInterpreter::findElementById(const char* id) {
    for (int i = 0; i < element_count; i++) {
        if (html_strcmp(dom_elements[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int HTMLInterpreter::findElementByTag(const char* tag) {
    for (int i = 0; i < element_count; i++) {
        if (html_strcmp(dom_elements[i].tag, tag) == 0) {
            return i;
        }
    }
    return -1;
}
