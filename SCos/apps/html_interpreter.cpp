
#include "html_interpreter.hpp"
#include "../ui/window_manager.hpp"
#include "../include/string.h"

static HTMLElement dom_elements[MAX_DOM_ELEMENTS];
static CSSRule css_rules[MAX_CSS_RULES];
static JSFunction js_functions[MAX_JS_FUNCTIONS];
static int element_count = 0;
static int css_rule_count = 0;
static int js_function_count = 0;

// Enhanced string utility functions
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

static void html_strcat(char* dest, const char* src) {
    dest += html_strlen(dest);
    html_strcpy(dest, src);
}

static void html_trim(char* str) {
    // Remove leading whitespace
    char* start = str;
    while (*start && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) {
        start++;
    }
    
    // Move string to beginning
    char* write = str;
    while (*start) {
        *write++ = *start++;
    }
    *write = '\0';
    
    // Remove trailing whitespace
    if (write > str) {
        write--;
        while (write >= str && (*write == ' ' || *write == '\t' || *write == '\n' || *write == '\r')) {
            *write-- = '\0';
        }
    }
}

static int html_atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

void HTMLInterpreter::init() {
    reset();
}

void HTMLInterpreter::reset() {
    element_count = 0;
    css_rule_count = 0;
    js_function_count = 0;

    for (int i = 0; i < MAX_DOM_ELEMENTS; i++) {
        dom_elements[i].tag[0] = '\0';
        dom_elements[i].id[0] = '\0';
        dom_elements[i].class_name[0] = '\0';
        dom_elements[i].content[0] = '\0';
        dom_elements[i].attributes[0] = '\0';
        dom_elements[i].visible = false;
        dom_elements[i].child_count = 0;
        dom_elements[i].parent_id = -1;
        dom_elements[i].x = 0;
        dom_elements[i].y = 0;
        dom_elements[i].width = 0;
        dom_elements[i].height = 1;
        dom_elements[i].color = 0x1F;
    }

    for (int i = 0; i < MAX_CSS_RULES; i++) {
        css_rules[i].selector[0] = '\0';
        css_rules[i].property[0] = '\0';
        css_rules[i].value[0] = '\0';
        css_rules[i].active = false;
    }

    for (int i = 0; i < MAX_JS_FUNCTIONS; i++) {
        js_functions[i].name[0] = '\0';
        js_functions[i].body[0] = '\0';
        js_functions[i].active = false;
    }
}

bool HTMLInterpreter::parseHTML(const char* html_content) {
    const char* pos = html_content;
    int current_parent = -1;
    int tag_stack[50];  // Stack to track nested elements
    int stack_depth = 0;

    while (*pos && element_count < MAX_DOM_ELEMENTS) {
        if (*pos == '<') {
            const char* tag_end = pos;
            while (*tag_end && *tag_end != '>') tag_end++;
            if (!*tag_end) break;

            // Extract tag content
            char tag_content[512];
            int tag_len = tag_end - pos - 1;
            if (tag_len > 511) tag_len = 511;

            for (int i = 0; i < tag_len; i++) {
                tag_content[i] = pos[i + 1];
            }
            tag_content[tag_len] = '\0';

            html_trim(tag_content);

            // Check if it's a closing tag
            if (tag_content[0] == '/') {
                // Pop from stack
                if (stack_depth > 0) {
                    stack_depth--;
                    current_parent = stack_depth > 0 ? tag_stack[stack_depth - 1] : -1;
                }
            } else if (tag_content[0] != '!' && tag_content[0] != '?') {
                // Opening tag or self-closing tag
                bool self_closing = (tag_content[tag_len - 1] == '/');
                if (self_closing) {
                    tag_content[tag_len - 1] = '\0';
                    html_trim(tag_content);
                }

                int element_id = parseHTMLElement(tag_content, current_parent);
                
                if (element_id >= 0 && !self_closing) {
                    // Check if this is a void element (self-closing by nature)
                    char tag_name[32];
                    extractTagName(tag_content, tag_name);
                    
                    if (!isVoidElement(tag_name)) {
                        // Push to stack for non-void elements
                        if (stack_depth < 50) {
                            tag_stack[stack_depth++] = element_id;
                            current_parent = element_id;
                        }
                    }
                }
            }

            pos = tag_end + 1;
        } else {
            // Text content
            const char* text_start = pos;
            while (*pos && *pos != '<') pos++;
            
            if (pos > text_start && current_parent >= 0) {
                // Add text content to current parent
                char text_content[256];
                int text_len = pos - text_start;
                if (text_len > 255) text_len = 255;
                
                for (int i = 0; i < text_len; i++) {
                    text_content[i] = text_start[i];
                }
                text_content[text_len] = '\0';
                
                html_trim(text_content);
                if (text_content[0] != '\0') {
                    html_strcat(dom_elements[current_parent].content, text_content);
                }
            }
            
            if (!*pos) break;
        }
    }

    calculateLayout();
    applyCSSRules();
    return true;
}

void HTMLInterpreter::extractTagName(const char* element_text, char* tag_name) {
    const char* space = element_text;
    while (*space && *space != ' ' && *space != '\t') space++;

    int tag_len = space - element_text;
    if (tag_len > 31) tag_len = 31;

    for (int i = 0; i < tag_len; i++) {
        tag_name[i] = element_text[i];
    }
    tag_name[tag_len] = '\0';
}

bool HTMLInterpreter::isVoidElement(const char* tag_name) {
    const char* void_elements[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    
    for (int i = 0; i < 14; i++) {
        if (html_strcmp(tag_name, void_elements[i]) == 0) {
            return true;
        }
    }
    return false;
}

int HTMLInterpreter::parseHTMLElement(const char* element_text, int parent_id) {
    if (element_count >= MAX_DOM_ELEMENTS) return -1;

    HTMLElement* element = &dom_elements[element_count];
    int element_id = element_count;

    // Extract tag name
    extractTagName(element_text, element->tag);

    // Parse attributes
    const char* space = element_text;
    while (*space && *space != ' ' && *space != '\t') space++;
    
    if (*space) {
        parseAttributes(space, element);
    }

    // Set defaults based on tag type
    element->visible = true;
    element->parent_id = parent_id;
    element->child_count = 0;
    element->color = 0x1F; // White on blue
    
    setDefaultElementProperties(element);

    // Add to parent's children
    if (parent_id >= 0 && parent_id < element_count) {
        HTMLElement* parent = &dom_elements[parent_id];
        if (parent->child_count < 10) {
            parent->children[parent->child_count++] = element_count;
        }
    }

    element_count++;
    return element_id;
}

void HTMLInterpreter::setDefaultElementProperties(HTMLElement* element) {
    if (html_strcmp(element->tag, "h1") == 0) {
        element->width = 60;
        element->height = 2;
        element->color = 0x4F; // Red
    } else if (html_strcmp(element->tag, "h2") == 0) {
        element->width = 55;
        element->height = 2;
        element->color = 0x2F; // Green
    } else if (html_strcmp(element->tag, "h3") == 0) {
        element->width = 50;
        element->height = 1;
        element->color = 0x6F; // Yellow
    } else if (html_strcmp(element->tag, "p") == 0) {
        element->width = 70;
        element->height = 1;
        element->color = 0x1F; // White
    } else if (html_strcmp(element->tag, "button") == 0) {
        element->width = 15;
        element->height = 1;
        element->color = 0x70; // Black on gray
    } else if (html_strcmp(element->tag, "input") == 0) {
        element->width = 20;
        element->height = 1;
        element->color = 0x0F; // White on black
    } else if (html_strcmp(element->tag, "div") == 0) {
        element->width = 75;
        element->height = 1;
        element->color = 0x1F; // White on blue
    } else if (html_strcmp(element->tag, "span") == 0) {
        element->width = 20;
        element->height = 1;
        element->color = 0x1F; // White on blue
    } else if (html_strcmp(element->tag, "ul") == 0 || html_strcmp(element->tag, "ol") == 0) {
        element->width = 70;
        element->height = 1;
        element->color = 0x1F; // White on blue
    } else if (html_strcmp(element->tag, "li") == 0) {
        element->width = 68;
        element->height = 1;
        element->color = 0x1F; // White on blue
    } else {
        element->width = 60;
        element->height = 1;
        element->color = 0x1F; // White on blue
    }
}

void HTMLInterpreter::parseAttributes(const char* attrs, HTMLElement* element) {
    const char* pos = attrs;
    
    while (*pos) {
        // Skip whitespace
        while (*pos && (*pos == ' ' || *pos == '\t')) pos++;
        if (!*pos) break;

        // Find attribute name
        const char* attr_start = pos;
        while (*pos && *pos != '=' && *pos != ' ' && *pos != '\t') pos++;
        
        if (*pos != '=') {
            // Skip this attribute if no value
            while (*pos && *pos != ' ' && *pos != '\t') pos++;
            continue;
        }

        // Extract attribute name
        char attr_name[32];
        int name_len = pos - attr_start;
        if (name_len > 31) name_len = 31;
        for (int i = 0; i < name_len; i++) {
            attr_name[i] = attr_start[i];
        }
        attr_name[name_len] = '\0';

        pos++; // Skip '='

        // Skip whitespace
        while (*pos && (*pos == ' ' || *pos == '\t')) pos++;

        // Find attribute value
        char quote_char = '\0';
        if (*pos == '"' || *pos == '\'') {
            quote_char = *pos++;
        }

        const char* value_start = pos;
        if (quote_char) {
            while (*pos && *pos != quote_char) pos++;
        } else {
            while (*pos && *pos != ' ' && *pos != '\t') pos++;
        }

        // Extract attribute value
        char attr_value[128];
        int value_len = pos - value_start;
        if (value_len > 127) value_len = 127;
        for (int i = 0; i < value_len; i++) {
            attr_value[i] = value_start[i];
        }
        attr_value[value_len] = '\0';

        if (quote_char && *pos) pos++; // Skip closing quote

        // Process specific attributes
        if (html_strcmp(attr_name, "id") == 0) {
            html_strcpy(element->id, attr_value);
        } else if (html_strcmp(attr_name, "class") == 0) {
            html_strcpy(element->class_name, attr_value);
        } else if (html_strcmp(attr_name, "style") == 0) {
            parseInlineStyle(attr_value, element);
        } else if (html_strcmp(attr_name, "width") == 0) {
            element->width = html_atoi(attr_value);
        } else if (html_strcmp(attr_name, "height") == 0) {
            element->height = html_atoi(attr_value);
        }
    }
}

void HTMLInterpreter::parseInlineStyle(const char* style, HTMLElement* element) {
    const char* pos = style;
    
    while (*pos) {
        // Skip whitespace
        while (*pos && (*pos == ' ' || *pos == '\t')) pos++;
        if (!*pos) break;

        // Find property name
        const char* prop_start = pos;
        while (*pos && *pos != ':') pos++;
        if (!*pos) break;

        char property[32];
        int prop_len = pos - prop_start;
        if (prop_len > 31) prop_len = 31;
        for (int i = 0; i < prop_len; i++) {
            property[i] = prop_start[i];
        }
        property[prop_len] = '\0';
        html_trim(property);

        pos++; // Skip ':'

        // Skip whitespace
        while (*pos && (*pos == ' ' || *pos == '\t')) pos++;

        // Find property value
        const char* value_start = pos;
        while (*pos && *pos != ';' && *pos != '\0') pos++;

        char value[64];
        int value_len = pos - value_start;
        if (value_len > 63) value_len = 63;
        for (int i = 0; i < value_len; i++) {
            value[i] = value_start[i];
        }
        value[value_len] = '\0';
        html_trim(value);

        if (*pos == ';') pos++;

        // Apply style property
        applyCSSProperty(element, property, value);
    }
}

void HTMLInterpreter::applyCSSProperty(HTMLElement* element, const char* property, const char* value) {
    if (html_strcmp(property, "color") == 0) {
        element->color = (element->color & 0xF0) | (parseColor(value) & 0x0F);
    } else if (html_strcmp(property, "background-color") == 0) {
        element->color = (element->color & 0x0F) | ((parseColor(value) & 0x0F) << 4);
    } else if (html_strcmp(property, "width") == 0) {
        element->width = html_atoi(value);
    } else if (html_strcmp(property, "height") == 0) {
        element->height = html_atoi(value);
    }
}

bool HTMLInterpreter::parseCSS(const char* css_content) {
    const char* pos = css_content;

    while (*pos && css_rule_count < MAX_CSS_RULES) {
        // Skip whitespace and comments
        while (*pos && (*pos == ' ' || *pos == '\n' || *pos == '\t' || *pos == '\r')) pos++;
        
        if (!*pos) break;

        // Skip CSS comments
        if (*pos == '/' && *(pos + 1) == '*') {
            pos += 2;
            while (*pos && !(*pos == '*' && *(pos + 1) == '/')) pos++;
            if (*pos) pos += 2;
            continue;
        }

        // Find selector (until {)
        const char* selector_start = pos;
        while (*pos && *pos != '{') pos++;
        if (!*pos) break;

        CSSRule* rule = &css_rules[css_rule_count];

        // Copy and clean selector
        int sel_len = pos - selector_start;
        if (sel_len > 63) sel_len = 63;
        for (int i = 0; i < sel_len; i++) {
            rule->selector[i] = selector_start[i];
        }
        rule->selector[sel_len] = '\0';
        html_trim(rule->selector);

        pos++; // Skip {

        // Parse properties until }
        while (*pos && *pos != '}') {
            // Skip whitespace
            while (*pos && (*pos == ' ' || *pos == '\n' || *pos == '\t' || *pos == '\r')) pos++;
            if (*pos == '}') break;

            // Get property
            const char* prop_start = pos;
            while (*pos && *pos != ':') pos++;
            if (!*pos || *pos != ':') break;

            int prop_len = pos - prop_start;
            if (prop_len > 31) prop_len = 31;
            for (int i = 0; i < prop_len; i++) {
                rule->property[i] = prop_start[i];
            }
            rule->property[prop_len] = '\0';
            html_trim(rule->property);

            pos++; // Skip :

            // Skip whitespace
            while (*pos && (*pos == ' ' || *pos == '\t')) pos++;

            // Get value
            const char* val_start = pos;
            while (*pos && *pos != ';' && *pos != '}') pos++;

            int val_len = pos - val_start;
            if (val_len > 63) val_len = 63;
            for (int i = 0; i < val_len; i++) {
                rule->value[i] = val_start[i];
            }
            rule->value[val_len] = '\0';
            html_trim(rule->value);

            if (*pos == ';') pos++;

            rule->active = true;
            css_rule_count++;

            if (css_rule_count >= MAX_CSS_RULES) break;
            
            // Prepare next rule with same selector
            if (css_rule_count < MAX_CSS_RULES) {
                rule = &css_rules[css_rule_count];
                html_strcpy(rule->selector, css_rules[css_rule_count-1].selector);
            }
        }

        if (*pos == '}') pos++;
    }

    return true;
}

bool HTMLInterpreter::parseJS(const char* js_content) {
    const char* pos = js_content;

    while (*pos && js_function_count < MAX_JS_FUNCTIONS) {
        // Skip whitespace and comments
        while (*pos && (*pos == ' ' || *pos == '\n' || *pos == '\t' || *pos == '\r')) pos++;
        
        if (!*pos) break;

        // Skip single line comments
        if (*pos == '/' && *(pos + 1) == '/') {
            while (*pos && *pos != '\n') pos++;
            continue;
        }

        // Skip multi-line comments
        if (*pos == '/' && *(pos + 1) == '*') {
            pos += 2;
            while (*pos && !(*pos == '*' && *(pos + 1) == '/')) pos++;
            if (*pos) pos += 2;
            continue;
        }

        // Look for function keyword
        if (html_strstr(pos, "function")) {
            pos += 8; // Skip "function"
            while (*pos && (*pos == ' ' || *pos == '\t')) pos++;

            // Get function name
            const char* name_start = pos;
            while (*pos && *pos != '(' && *pos != ' ' && *pos != '\t') pos++;

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
            
            if (matchesSelector(element, rule->selector)) {
                applyCSSProperty(element, rule->property, rule->value);
            }
        }
    }
}

bool HTMLInterpreter::matchesSelector(HTMLElement* element, const char* selector) {
    if (selector[0] == '#') {
        // ID selector
        return html_strcmp(selector + 1, element->id) == 0;
    } else if (selector[0] == '.') {
        // Class selector
        return html_strstr(element->class_name, selector + 1);
    } else {
        // Tag selector or compound selector
        char tag_selector[64];
        html_strcpy(tag_selector, selector);
        
        // Handle compound selectors (space-separated)
        char* space = tag_selector;
        while (*space && *space != ' ') space++;
        if (*space) *space = '\0';
        
        return html_strcmp(tag_selector, element->tag) == 0;
    }
}

uint8_t HTMLInterpreter::parseColor(const char* color_name) {
    if (html_strcmp(color_name, "red") == 0) return 4;
    if (html_strcmp(color_name, "green") == 0) return 2;
    if (html_strcmp(color_name, "blue") == 0) return 1;
    if (html_strcmp(color_name, "yellow") == 0) return 6;
    if (html_strcmp(color_name, "cyan") == 0) return 3;
    if (html_strcmp(color_name, "magenta") == 0) return 5;
    if (html_strcmp(color_name, "white") == 0) return 15;
    if (html_strcmp(color_name, "black") == 0) return 0;
    if (html_strcmp(color_name, "gray") == 0 || html_strcmp(color_name, "grey") == 0) return 8;
    
    // Handle hex colors (#RGB or #RRGGBB)
    if (color_name[0] == '#') {
        // Simple hex to VGA color mapping
        return 7; // Default to light gray
    }
    
    return 15; // Default white
}

void HTMLInterpreter::calculateLayout() {
    int current_y = 0;
    int margin_left = 2;

    for (int i = 0; i < element_count; i++) {
        HTMLElement* element = &dom_elements[i];

        if (element->parent_id == -1) { // Root level elements
            element->x = margin_left;
            element->y = current_y + 2;
            current_y += element->height + getElementSpacing(element);
        } else {
            // Child elements - position relative to parent
            HTMLElement* parent = &dom_elements[element->parent_id];
            
            if (html_strcmp(element->tag, "li") == 0) {
                // List items get indented
                element->x = parent->x + 4;
                element->y = parent->y + parent->height + (element - dom_elements - element->parent_id - 1);
            } else {
                // Regular child positioning
                element->x = parent->x + 2;
                element->y = parent->y + parent->height + 1;
            }
        }

        // Ensure elements don't go off screen
        if (element->x + element->width > 78) {
            element->width = 78 - element->x;
        }
        if (element->y > 23) {
            element->visible = false; // Hide elements that would be off-screen
        }
    }
}

int HTMLInterpreter::getElementSpacing(HTMLElement* element) {
    if (html_strcmp(element->tag, "h1") == 0) return 2;
    if (html_strcmp(element->tag, "h2") == 0) return 1;
    if (html_strcmp(element->tag, "h3") == 0) return 1;
    if (html_strcmp(element->tag, "p") == 0) return 1;
    if (html_strcmp(element->tag, "div") == 0) return 1;
    if (html_strcmp(element->tag, "ul") == 0 || html_strcmp(element->tag, "ol") == 0) return 1;
    return 0;
}

void HTMLInterpreter::renderPage(int window_id) {
    Window* win = WindowManager::getWindow(window_id);
    if (!win) return;

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

    // Determine what to display
    const char* display_text = element->content[0] ? element->content : element->tag;
    
    // Special rendering for certain elements
    char prefix[8] = "";
    if (html_strcmp(element->tag, "li") == 0) {
        html_strcpy(prefix, "• ");
    } else if (html_strcmp(element->tag, "button") == 0) {
        html_strcpy(prefix, "[");
    }

    // Render prefix
    int x_offset = 0;
    for (int i = 0; prefix[i] && x_offset < element->width; i++) {
        if (screen_x + x_offset >= 0 && screen_x + x_offset < 80 && 
            screen_y >= 0 && screen_y < 25) {
            int idx = 2 * (screen_y * 80 + screen_x + x_offset);
            video[idx] = prefix[i];
            video[idx + 1] = element->color;
        }
        x_offset++;
    }

    // Render main content
    for (int i = 0; display_text[i] && x_offset < element->width && 
         screen_x + x_offset < win->x + win->width; i++) {
        if (screen_x + x_offset >= 0 && screen_x + x_offset < 80 && 
            screen_y >= 0 && screen_y < 25) {
            int idx = 2 * (screen_y * 80 + screen_x + x_offset);
            video[idx] = display_text[i];
            video[idx + 1] = element->color;
        }
        x_offset++;
    }

    // Add suffix for buttons
    if (html_strcmp(element->tag, "button") == 0 && x_offset < element->width) {
        if (screen_x + x_offset >= 0 && screen_x + x_offset < 80 && 
            screen_y >= 0 && screen_y < 25) {
            int idx = 2 * (screen_y * 80 + screen_x + x_offset);
            video[idx] = ']';
            video[idx + 1] = element->color;
        }
    }
}

void HTMLInterpreter::handleClick(int x, int y) {
    // Find clicked element and execute any associated JS
    for (int i = 0; i < element_count; i++) {
        HTMLElement* element = &dom_elements[i];
        if (element->visible &&
            x >= element->x && x < element->x + element->width &&
            y >= element->y && y < element->y + element->height) {

            // Look for onclick handler
            if (html_strcmp(element->tag, "button") == 0) {
                char onclick_func[128];
                if (element->id[0] != '\0') {
                    html_strcpy(onclick_func, element->id);
                    html_strcat(onclick_func, "_click");
                } else {
                    html_strcpy(onclick_func, "button_click");
                }
                executeJS(onclick_func);
            }
            break;
        }
    }
}

void HTMLInterpreter::executeJS(const char* function_name) {
    for (int i = 0; i < js_function_count; i++) {
        if (js_functions[i].active && html_strcmp(js_functions[i].name, function_name) == 0) {
            // Simple JS execution - this is a basic implementation
            // In a full implementation, this would parse and execute the JS
            
            // For now, we can handle some basic functions
            if (html_strstr(js_functions[i].body, "alert")) {
                // Simple alert simulation - could flash the screen or show a message
                flashScreen();
            }
            break;
        }
    }
}

void HTMLInterpreter::flashScreen() {
    volatile char* video = (volatile char*)0xB8000;
    
    // Save current colors
    static uint8_t saved_colors[80 * 25];
    for (int i = 0; i < 80 * 25; i++) {
        saved_colors[i] = video[i * 2 + 1];
    }
    
    // Flash white
    for (int i = 0; i < 80 * 25; i++) {
        video[i * 2 + 1] = 0xFF;
    }
    
    // Simple delay
    for (volatile int i = 0; i < 1000000; i++);
    
    // Restore colors
    for (int i = 0; i < 80 * 25; i++) {
        video[i * 2 + 1] = saved_colors[i];
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

HTMLElement* HTMLInterpreter::getElementById(const char* id) {
    int index = findElementById(id);
    return index >= 0 ? &dom_elements[index] : nullptr;
}

void HTMLInterpreter::updateElementContent(const char* id, const char* new_content) {
    HTMLElement* element = getElementById(id);
    if (element) {
        html_strcpy(element->content, new_content);
    }
}

void HTMLInterpreter::toggleElementVisibility(const char* id) {
    HTMLElement* element = getElementById(id);
    if (element) {
        element->visible = !element->visible;
    }
}
