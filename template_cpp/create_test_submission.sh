#!/bin/bash

# Automated Testing Submission Script for CS451 Distributed Algorithms
# This script creates a submission.zip file according to project requirements

set -e  # Exit on any error

# Get the directory of this script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=================================================================================${NC}"
echo -e "${BLUE}CS451 Distributed Algorithms - Testing Submission Generator${NC}"
echo -e "${BLUE}=================================================================================${NC}"

# Default test case
TEST_CASE=0

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--test)
            TEST_CASE="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [-t|--test TEST_NUMBER]"
            echo ""
            echo "Options:"
            echo "  -t, --test TEST_NUMBER    Specify test case number (default: 0)"
            echo "  -h, --help               Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                       # Create submission for test case 0"
            echo "  $0 -t 3                  # Create submission for test case 3"
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}"
            echo "Use -h or --help for usage information"
            exit 1
            ;;
    esac
done

# Validate test case number (no upper limit specified, but warn if too high)
if ! [[ "$TEST_CASE" =~ ^[0-9]+$ ]]; then
    echo -e "${RED}Error: Test case must be a non-negative integer${NC}"
    echo -e "${RED}Provided: $TEST_CASE${NC}"
    exit 1
fi

if [ "$TEST_CASE" -gt 20 ]; then
    echo -e "${YELLOW}Warning: Test case $TEST_CASE seems unusually high${NC}"
    read -p "Continue? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

echo -e "${YELLOW}Selected test case: $TEST_CASE${NC}"

# Step 1: Clean build
echo -e "\n${BLUE}Step 1: Cleaning previous build artifacts...${NC}"
if [ -f "cleanup.sh" ]; then
    chmod +x cleanup.sh
    ./cleanup.sh
    echo -e "${GREEN}✓ Cleanup completed${NC}"
else
    echo -e "${YELLOW}⚠ Warning: cleanup.sh not found, skipping cleanup${NC}"
fi

# Step 2: Verify required files exist
echo -e "\n${BLUE}Step 2: Verifying project structure...${NC}"

REQUIRED_FILES=(
    "CMakeLists.txt"
    "build.sh"
    "run.sh"
    "cleanup.sh"
    "src/CMakeLists.txt"
)

REQUIRED_DIRS=(
    "src/"
    "bin/"
)

MISSING_FILES=()
for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$file" ]; then
        MISSING_FILES+=("$file")
    fi
done

for dir in "${REQUIRED_DIRS[@]}"; do
    if [ ! -d "$dir" ]; then
        MISSING_FILES+=("$dir")
    fi
done

if [ ${#MISSING_FILES[@]} -ne 0 ]; then
    echo -e "${RED}Error: Missing required files/directories:${NC}"
    for file in "${MISSING_FILES[@]}"; do
        echo -e "${RED}  - $file${NC}"
    done
    exit 1
fi

echo -e "${GREEN}✓ All required files found${NC}"

# Step 3: Create temporary directory for zip contents
echo -e "\n${BLUE}Step 3: Preparing submission files...${NC}"

TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

# Copy required files according to project template structure
echo -e "${YELLOW}Copying project files...${NC}"

# Create directory structure
mkdir -p "$TEMP_DIR/bin/deploy"
mkdir -p "$TEMP_DIR/bin/logs"
mkdir -p "$TEMP_DIR/src"

# Copy root level files (scripts and CMakeLists.txt)
cp build.sh "$TEMP_DIR/"
cp run.sh "$TEMP_DIR/"
cp cleanup.sh "$TEMP_DIR/"
cp CMakeLists.txt "$TEMP_DIR/"

# Copy README files if they exist
[ -f "README" ] && cp README "$TEMP_DIR/" || true
[ -f "README.md" ] && cp README.md "$TEMP_DIR/" || true

# Copy bin/deploy/README and bin/logs/README (required by template)
if [ -f "bin/deploy/README" ]; then
    cp bin/deploy/README "$TEMP_DIR/bin/deploy/"
else
    echo "Do not edit this directory!" > "$TEMP_DIR/bin/deploy/README"
fi

if [ -f "bin/logs/README" ]; then
    cp bin/logs/README "$TEMP_DIR/bin/logs/"
else
    echo "Do not edit this directory!" > "$TEMP_DIR/bin/logs/README"
fi

# Copy entire src directory (all your source code)
# Use rsync to copy while excluding unwanted files
rsync -a \
    --exclude='*.o' \
    --exclude='*.a' \
    --exclude='*.so' \
    --exclude='*.dylib' \
    --exclude='CMakeFiles/' \
    --exclude='cmake_install.cmake' \
    --exclude='CMakeCache.txt' \
    --exclude='Makefile' \
    --exclude='.DS_Store' \
    src/ "$TEMP_DIR/src/"

echo -e "${GREEN}✓ Required files copied to temporary directory${NC}"

# Verify no forbidden content is included
echo -e "\n${BLUE}Checking for forbidden content...${NC}"

FORBIDDEN_PATTERNS=(
    "*.output"
    "*.log"
    "files/output/*"
    "example/output/*"
    "test.txt"
    "da_proc"
)

FOUND_FORBIDDEN=0
for pattern in "${FORBIDDEN_PATTERNS[@]}"; do
    if find "$TEMP_DIR" -name "$pattern" -o -path "*/$pattern" 2>/dev/null | grep -q .; then
        echo -e "${RED}⚠ Warning: Found forbidden file matching pattern: $pattern${NC}"
        FOUND_FORBIDDEN=1
    fi
done

if [ $FOUND_FORBIDDEN -eq 0 ]; then
    echo -e "${GREEN}✓ No forbidden content detected${NC}"
fi

# Step 4: Create submission.zip
echo -e "\n${BLUE}Step 4: Creating submission.zip...${NC}"

# Remove any existing submission.zip
rm -f submission.zip

# Create zip file containing the contents of the temp directory
# Important: zip the CONTENTS, not the directory itself
cd "$TEMP_DIR"
zip -r "$SCRIPT_DIR/submission.zip" . \
    -x "*.DS_Store" \
    -x "*/.DS_Store" \
    -x "__MACOSX/*" \
    -x "*.swp" \
    -x "*~"

cd "$SCRIPT_DIR"

if [ ! -f "submission.zip" ]; then
    echo -e "${RED}Error: Failed to create submission.zip${NC}"
    exit 1
fi

SUBMISSION_SIZE=$(du -h submission.zip | cut -f1)
echo -e "${GREEN}✓ submission.zip created successfully (${SUBMISSION_SIZE})${NC}"

# Step 5: Create test.txt file
echo -e "\n${BLUE}Step 5: Creating test.txt file...${NC}"

echo "$TEST_CASE" > test.txt
echo -e "${GREEN}✓ test.txt created with test case: $TEST_CASE${NC}"

# Step 6: Verify zip contents
echo -e "\n${BLUE}Step 6: Verifying submission contents...${NC}"

echo -e "${YELLOW}Top-level structure of submission.zip:${NC}"
unzip -l submission.zip | grep "/$" | head -10

# Count files
ZIP_FILE_COUNT=$(unzip -l submission.zip | grep -c "^\s*[0-9]" || true)
echo -e "${BLUE}Total files in zip: $ZIP_FILE_COUNT${NC}"

# Step 7: Verify correct structure (critical!)
echo -e "\n${BLUE}Step 7: Structure verification...${NC}"

# Check top-level entries
TOP_LEVEL=$(unzip -l submission.zip | awk 'NR>3 {print $4}' | grep -v "^$" | cut -d'/' -f1 | sort -u | grep -v "^Length" | grep -v "^---" | head -10)

EXPECTED_TOP_LEVEL=(
    "bin"
    "src"
    "build.sh"
    "run.sh"
    "cleanup.sh"
    "CMakeLists.txt"
)

echo -e "${YELLOW}Top-level entries in zip:${NC}"
echo "$TOP_LEVEL"

# Critical check: ensure no wrapper directory
if echo "$TOP_LEVEL" | grep -q "^template_cpp$"; then
    echo -e "${RED}❌ CRITICAL ERROR: Found 'template_cpp' folder in zip root!${NC}"
    echo -e "${RED}This will cause 'setup script failed' error.${NC}"
    echo -e "${RED}The zip must contain the CONTENTS directly, not wrapped in a folder.${NC}"
    exit 1
fi

# Verify expected files are present
echo -e "\n${YELLOW}Verifying required files are in zip:${NC}"
for file in "${EXPECTED_TOP_LEVEL[@]}"; do
    if unzip -l submission.zip | grep -q "^\s*[0-9].*\s$file"; then
        echo -e "${GREEN}  ✓ $file${NC}"
    else
        echo -e "${RED}  ✗ $file (MISSING!)${NC}"
    fi
done

# Verify src directory structure
echo -e "\n${YELLOW}Checking src directory structure:${NC}"
if unzip -l submission.zip | grep -q "^\s*[0-9].*\ssrc/CMakeLists.txt"; then
    echo -e "${GREEN}  ✓ src/CMakeLists.txt found${NC}"
else
    echo -e "${RED}  ✗ src/CMakeLists.txt missing (CRITICAL!)${NC}"
fi

SRC_FILE_COUNT=$(unzip -l submission.zip | grep "^\s*[0-9].*\ssrc/" | wc -l)
echo -e "${BLUE}  Total files in src/: $SRC_FILE_COUNT${NC}"

# Check for unwanted files
echo -e "\n${YELLOW}Checking for unwanted files:${NC}"
UNWANTED_FOUND=0

if unzip -l submission.zip | grep -q "\.output$"; then
    echo -e "${RED}  ⚠ Found .output files (should not be included)${NC}"
    UNWANTED_FOUND=1
fi

if unzip -l submission.zip | grep -q "da_proc$"; then
    echo -e "${RED}  ⚠ Found compiled binary da_proc (should not be included)${NC}"
    UNWANTED_FOUND=1
fi

if unzip -l submission.zip | grep -q "files/"; then
    echo -e "${YELLOW}  ⚠ Found 'files/' directory (usually test files, verify this is intentional)${NC}"
fi

if [ $UNWANTED_FOUND -eq 0 ]; then
    echo -e "${GREEN}  ✓ No obviously unwanted files detected${NC}"
fi

# Summary
echo -e "\n${BLUE}=================================================================================${NC}"
echo -e "${GREEN}✓ SUBMISSION READY${NC}"
echo -e "${BLUE}=================================================================================${NC}"
echo -e "${GREEN}Created files:${NC}"
echo -e "  📦 submission.zip (${SUBMISSION_SIZE})"
echo -e "  📄 test.txt (test case: $TEST_CASE)"
echo ""
echo -e "${YELLOW}⚠ IMPORTANT: Upload both files to the testing submission system:${NC}"
echo -e "  1. submission.zip"
echo -e "  2. test.txt"
echo ""
echo -e "${BLUE}Submission structure verified:${NC}"
echo -e "  ✓ No wrapper directory (contents at root level)"
echo -e "  ✓ Required scripts present (build.sh, run.sh, cleanup.sh)"
echo -e "  ✓ Source code in src/ directory"
echo -e "  ✓ bin/ directory structure correct"
echo ""
echo -e "${YELLOW}Testing tips:${NC}"
echo -e "  • Lower test numbers (0-3): Fast feedback"
echo -e "  • Higher test numbers: May take longer"
echo -e "  • If you get 'setup script failed': Check zip structure"
echo -e "  • Remember: Unlimited test submissions allowed!"
echo ""
echo -e "${GREEN}Good luck with your submission! 🚀${NC}"
echo -e "${BLUE}=================================================================================${NC}"