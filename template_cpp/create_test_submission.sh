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
            echo "  -t, --test TEST_NUMBER    Specify test case number (0-6, default: 0)"
            echo "  -h, --help               Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                       # Create submission for test case 0"
            echo "  $0 -t 3                  # Create submission for test case 3"
            echo "  $0 --test 6              # Create submission for test case 6"
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}"
            echo "Use -h or --help for usage information"
            exit 1
            ;;
    esac
done

# Validate test case number
if ! [[ "$TEST_CASE" =~ ^[0-6]$ ]]; then
    echo -e "${RED}Error: Test case must be a number between 0 and 6${NC}"
    echo -e "${RED}Provided: $TEST_CASE${NC}"
    exit 1
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
    "src/"
)

MISSING_FILES=()
for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -e "$file" ]; then
        MISSING_FILES+=("$file")
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

# Copy all files from template_cpp (current directory) to temp directory
# Exclude certain files/directories that shouldn't be in submission
EXCLUDE_PATTERNS=(
    "*.zip"
    "test.txt"
    "submission.zip"
    ".git*"
    "*.log"
    "*.tmp"
    "bin/da_proc"
    "bin/logs/*"
    "bin/deploy/*"
    ".vscode/"
    ".idea/"
    "target/"
)

echo -e "${YELLOW}Copying project files...${NC}"

# Use rsync for better control over what gets copied
rsync -av \
    --exclude='*.zip' \
    --exclude='test.txt' \
    --exclude='submission.zip' \
    --exclude='.git*' \
    --exclude='*.log' \
    --exclude='*.tmp' \
    --exclude='bin/da_proc' \
    --exclude='bin/logs/*' \
    --exclude='bin/deploy/*' \
    --exclude='.vscode/' \
    --exclude='.idea/' \
    --exclude='target/' \
    ./ "$TEMP_DIR/"

echo -e "${GREEN}✓ Files copied to temporary directory${NC}"

# Step 4: Create submission.zip
echo -e "\n${BLUE}Step 4: Creating submission.zip...${NC}"

# Remove any existing submission.zip
rm -f submission.zip

# Create zip file containing the contents of the temp directory
cd "$TEMP_DIR"
zip -r "$SCRIPT_DIR/submission.zip" . -x "*.DS_Store" "*/.DS_Store"

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

echo -e "${YELLOW}Contents of submission.zip:${NC}"
unzip -l submission.zip | head -20

ZIP_FILE_COUNT=$(unzip -l submission.zip | grep -c "^\s*[0-9]" || true)
echo -e "${BLUE}Total files in zip: $ZIP_FILE_COUNT${NC}"

# Step 7: Final verification
echo -e "\n${BLUE}Step 7: Final verification...${NC}"

# Check if zip has the correct structure (no top-level template_cpp folder)
TOP_LEVEL_DIRS=$(unzip -l submission.zip | awk '/^[[:space:]]*[0-9]/ {print $4}' | cut -d'/' -f1 | sort -u | head -5)
echo -e "${YELLOW}Top-level directories in zip:${NC}"
echo "$TOP_LEVEL_DIRS"

if echo "$TOP_LEVEL_DIRS" | grep -q "^template_cpp$"; then
    echo -e "${RED}⚠ Warning: Found template_cpp folder in zip root. This may cause 'setup script failed' error.${NC}"
    echo -e "${RED}The zip should contain the CONTENTS of template_cpp, not the folder itself.${NC}"
fi

# Summary
echo -e "\n${BLUE}=================================================================================${NC}"
echo -e "${GREEN}✓ SUBMISSION READY${NC}"
echo -e "${BLUE}=================================================================================${NC}"
echo -e "${GREEN}Created files:${NC}"
echo -e "  📦 submission.zip (${SUBMISSION_SIZE})"
echo -e "  📄 test.txt (test case: $TEST_CASE)"
echo ""
echo -e "${YELLOW}Upload both files to the testing submission system:${NC}"
echo -e "  1. submission.zip"
echo -e "  2. test.txt"
echo ""
echo -e "${BLUE}Test cases available:${NC}"
echo -e "  0-3: Fast feedback"
echo -e "  4-6: May take longer"
echo ""
echo -e "${GREEN}Good luck with your submission! 🚀${NC}"