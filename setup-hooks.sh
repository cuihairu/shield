#!/bin/bash

# Setup git hooks for the project
HOOKS_DIR=".git/hooks"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Setting up git hooks..."

# Create pre-commit hook
cat > "$PROJECT_ROOT/$HOOKS_DIR/pre-commit" << 'EOF'
#!/bin/bash

# Get list of staged C++ files
files=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(cpp|c|h|hpp|cc|cxx)$')

if [ ! -z "$files" ]; then
    echo "Running clang-format on staged files..."
    
    # Check if clang-format is available
    if ! command -v clang-format &> /dev/null; then
        echo "Error: clang-format not found. Please install clang-format."
        exit 1
    fi
    
    # Format the files
    echo "$files" | xargs clang-format -i
    
    # Add the formatted files back to staging
    echo "$files" | xargs git add
    
    echo "Code formatting completed."
fi

exit 0
EOF

# Make hook executable
chmod +x "$PROJECT_ROOT/$HOOKS_DIR/pre-commit"

echo "✓ Pre-commit hook installed successfully!"
echo "  Hook location: $PROJECT_ROOT/$HOOKS_DIR/pre-commit"
echo ""
echo "The hook will automatically format C++ files before each commit."
echo "To skip formatting for a specific commit, use: git commit --no-verify"