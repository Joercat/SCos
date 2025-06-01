
// SCos Browser JavaScript Functions

// Test function for button interaction
function testFunction() {
    alert('Hello from SCos Browser!\n\nThis demonstrates JavaScript execution in the SCos web browser.');
    
    // Change the button text
    var button = document.getElementById('test-button');
    if (button) {
        button.textContent = 'Clicked!';
        button.style.backgroundColor = '#ff0000';
    }
}

// Function to show input text
function showText() {
    var input = document.getElementById('text-input');
    if (input && input.value) {
        alert('You entered: ' + input.value);
    } else {
        alert('Please enter some text first!');
    }
}

// Function to demonstrate DOM manipulation
function changePageStyle() {
    var title = document.getElementById('main-title');
    if (title) {
        title.style.color = '#ff0080';
        title.textContent = 'SCos Browser - JavaScript Active!';
    }
}

// Function to create dynamic content
function addContent() {
    var main = document.querySelector('main');
    if (main) {
        var newSection = document.createElement('section');
        newSection.innerHTML = '<h3>Dynamic Content</h3><p>This content was added by JavaScript!</p>';
        newSection.style.backgroundColor = '#008080';
        newSection.style.padding = '15px';
        newSection.style.marginTop = '20px';
        main.appendChild(newSection);
    }
}

// Event listeners for page interaction
document.addEventListener('DOMContentLoaded', function() {
    console.log('SCos Browser JavaScript loaded successfully!');
    
    // Add click handler to title
    var title = document.getElementById('main-title');
    if (title) {
        title.addEventListener('click', changePageStyle);
        title.style.cursor = 'pointer';
        title.title = 'Click to change style';
    }
});

// Simple utility functions
function showAlert(message) {
    alert('SCos Alert: ' + message);
}

function toggleElement(elementId) {
    var element = document.getElementById(elementId);
    if (element) {
        element.style.display = element.style.display === 'none' ? 'block' : 'none';
    }
}

// Browser-specific functions
function reloadPage() {
    window.location.reload();
}

function navigateToPage(url) {
    window.location.href = url;
}

// Demonstrate modern JavaScript features
const browserInfo = {
    name: 'SCos Browser',
    version: '1.0',
    features: ['HTML5', 'CSS3', 'JavaScript ES6+', 'File System Access'],
    
    displayInfo() {
        return `${this.name} v${this.version}\nFeatures: ${this.features.join(', ')}`;
    }
};

// Arrow function example
const greetUser = (name = 'User') => {
    return `Hello, ${name}! Welcome to SCos Browser.`;
};

// Promise example for async operations
function loadData() {
    return new Promise((resolve, reject) => {
        setTimeout(() => {
            resolve('Data loaded successfully!');
        }, 1000);
    });
}

// Usage example
loadData().then(result => {
    console.log(result);
}).catch(error => {
    console.error('Error loading data:', error);
});
