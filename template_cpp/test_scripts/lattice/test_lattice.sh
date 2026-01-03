#!/bin/bash

################################################################################
# Lattice Agreement Local Testing Script
# 5-Layer Progressive Testing Strategy
################################################################################

set -e

# ==================== Configuration ====================

# Test layer control switches (1=enable, 0=disable)
RUN_LAYER_1_BASIC=1         # Basic functionality test
RUN_LAYER_2_DUAL=1          # Dual-process protocol test
RUN_LAYER_3_TRI=1           # Three-process standard test
RUN_LAYER_4_MULTI_SLOT=1    # Multi-slot test
RUN_LAYER_5_STRESS=1        # Stress test

# Timeout settings (seconds)
TIMEOUT_BASIC=10
TIMEOUT_STRESS=60

# Paths
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$SCRIPT_DIR/../.."
BIN_PATH="$PROJECT_ROOT/bin/da_proc"
TEST_DIR="$SCRIPT_DIR/test_runs"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ==================== Helper Functions ====================

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

cleanup() {
    log_info "Cleaning up processes..."
    pkill -9 da_proc 2>/dev/null || true
    sleep 1
}

setup_test_dir() {
    local test_name=$1
    local test_path="$TEST_DIR/$test_name"
    rm -rf "$test_path"
    mkdir -p "$test_path/output"
    echo "$test_path"
}

create_hosts_file() {
    local test_path=$1
    local n_processes=$2
    local base_port=11001

    > "$test_path/hosts"
    for ((i=1; i<=n_processes; i++)); do
        echo "$i 127.0.0.1 $((base_port + i - 1))" >> "$test_path/hosts"
    done
}

create_config_file() {
    local test_path=$1
    shift
    local proposals=("$@")

    local n_proposals=${#proposals[@]}
    local max_values=0
    local distinct_values=0

    # Calculate max_values and distinct_values
    local -A distinct_set
    for prop in "${proposals[@]}"; do
        local count=$(echo "$prop" | wc -w)
        if [ $count -gt $max_values ]; then
            max_values=$count
        fi
        for val in $prop; do
            distinct_set[$val]=1
        done
    done
    distinct_values=${#distinct_set[@]}

    # Write config file
    echo "$n_proposals $max_values $distinct_values" > "$test_path/config"
    for prop in "${proposals[@]}"; do
        echo "$prop" >> "$test_path/config"
    done
}

start_process() {
    local test_path=$1
    local process_id=$2
    local timeout=$3

    local output_file="$test_path/output/${process_id}.output"
    local stdout_file="$test_path/output/${process_id}.stdout"
    local stderr_file="$test_path/output/${process_id}.stderr"

    timeout $timeout "$BIN_PATH" \
        --id $process_id \
        --hosts "$test_path/hosts" \
        --output "$output_file" \
        "$test_path/config" \
        > "$stdout_file" 2> "$stderr_file" &

    echo $!
}

wait_and_signal() {
    local pids=("$@")
    local wait_time=${pids[-1]}
    unset 'pids[-1]'

    sleep $wait_time

    for pid in "${pids[@]}"; do
        if kill -0 $pid 2>/dev/null; then
            kill -TERM $pid 2>/dev/null || true
        fi
    done

    sleep 1

    for pid in "${pids[@]}"; do
        if kill -0 $pid 2>/dev/null; then
            kill -9 $pid 2>/dev/null || true
        fi
    done

    wait 2>/dev/null || true
}

check_output_exists() {
    local test_path=$1
    local n_processes=$2

    for ((i=1; i<=n_processes; i++)); do
        local output_file="$test_path/output/${i}.output"
        if [ ! -f "$output_file" ]; then
            log_error "Output file missing for process $i"
            return 1
        fi
        if [ ! -s "$output_file" ]; then
            log_error "Output file empty for process $i"
            return 1
        fi
    done
    return 0
}

verify_validity() {
    local test_path=$1
    local n_processes=$2
    local -n proposals_ref=$3

    for ((i=1; i<=n_processes; i++)); do
        local output_file="$test_path/output/${i}.output"
        local line_num=1

        while IFS= read -r decision_line; do
            local decision_set=($decision_line)
            local proposal_set=(${proposals_ref[$((i-1))]})

            # Check: proposal ⊆ decision
            for val in "${proposal_set[@]}"; do
                if [[ ! " ${decision_set[@]} " =~ " ${val} " ]]; then
                    log_error "Process $i, slot $((line_num-1)): decision does not contain proposal value $val"
                    return 1
                fi
            done

            ((line_num++))
        done < "$output_file"
    done

    return 0
}

verify_consistency() {
    local test_path=$1
    local n_processes=$2
    local n_proposals=$3

    for ((slot=0; slot<n_proposals; slot++)); do
        # Collect all decisions for this slot
        local -a all_decisions
        for ((i=1; i<=n_processes; i++)); do
            local output_file="$test_path/output/${i}.output"
            local decision_line=$(sed -n "$((slot+1))p" "$output_file")
            if [ -n "$decision_line" ]; then
                all_decisions+=("$decision_line")
            fi
        done

        # Check pairwise comparability
        local n_decisions=${#all_decisions[@]}
        for ((i=0; i<n_decisions; i++)); do
            for ((j=i+1; j<n_decisions; j++)); do
                local set1=(${all_decisions[$i]})
                local set2=(${all_decisions[$j]})

                # Check if set1 ⊆ set2 or set2 ⊆ set1
                local is_subset_1_2=1
                local is_subset_2_1=1

                for val in "${set1[@]}"; do
                    if [[ ! " ${set2[@]} " =~ " ${val} " ]]; then
                        is_subset_1_2=0
                        break
                    fi
                done

                for val in "${set2[@]}"; do
                    if [[ ! " ${set1[@]} " =~ " ${val} " ]]; then
                        is_subset_2_1=0
                        break
                    fi
                done

                if [ $is_subset_1_2 -eq 0 ] && [ $is_subset_2_1 -eq 0 ]; then
                    log_error "Slot $slot: decisions are not comparable"
                    log_error "  Decision 1: ${all_decisions[$i]}"
                    log_error "  Decision 2: ${all_decisions[$j]}"
                    return 1
                fi
            done
        done
    done

    return 0
}

verify_termination() {
    local test_path=$1
    local n_processes=$2
    local expected_lines=$3

    for ((i=1; i<=n_processes; i++)); do
        local output_file="$test_path/output/${i}.output"
        local actual_lines=$(wc -l < "$output_file")

        if [ $actual_lines -ne $expected_lines ]; then
            log_warning "Process $i: expected $expected_lines decisions, got $actual_lines"
        fi
    done

    return 0
}

# ==================== Test Layers ====================

test_layer_1_basic() {
    log_info "========================================"
    log_info "Layer 1: Basic Functionality Test"
    log_info "========================================"

    local test_path=$(setup_test_dir "layer1_basic")
    local n_processes=3

    create_hosts_file "$test_path" $n_processes
    create_config_file "$test_path" "1"

    log_info "Starting $n_processes processes..."
    local pids=()
    for ((i=1; i<=n_processes; i++)); do
        local pid=$(start_process "$test_path" $i $TIMEOUT_BASIC)
        pids+=($pid)
    done

    wait_and_signal "${pids[@]}" 3

    log_info "Verifying outputs..."
    if ! check_output_exists "$test_path" $n_processes; then
        log_error "Layer 1 FAILED: Output files missing or empty"
        return 1
    fi

    local proposals=("1" "1" "1")
    if ! verify_validity "$test_path" $n_processes proposals; then
        log_error "Layer 1 FAILED: Validity check failed"
        return 1
    fi

    if ! verify_consistency "$test_path" $n_processes 1; then
        log_error "Layer 1 FAILED: Consistency check failed"
        return 1
    fi

    verify_termination "$test_path" $n_processes 1

    log_success "Layer 1 PASSED"
    return 0
}

test_layer_2_dual() {
    log_info "========================================"
    log_info "Layer 2: Dual-Process Protocol Test"
    log_info "========================================"

    local test_path=$(setup_test_dir "layer2_dual")
    local n_processes=2

    create_hosts_file "$test_path" $n_processes
    create_config_file "$test_path" "1" "2"

    log_info "Starting $n_processes processes with different proposals..."
    local pids=()
    for ((i=1; i<=n_processes; i++)); do
        local pid=$(start_process "$test_path" $i $TIMEOUT_BASIC)
        pids+=($pid)
    done

    wait_and_signal "${pids[@]}" 3

    log_info "Verifying outputs..."
    if ! check_output_exists "$test_path" $n_processes; then
        log_error "Layer 2 FAILED: Output files missing or empty"
        return 1
    fi

    local proposals=("1" "2")
    if ! verify_validity "$test_path" $n_processes proposals; then
        log_error "Layer 2 FAILED: Validity check failed"
        return 1
    fi

    if ! verify_consistency "$test_path" $n_processes 1; then
        log_error "Layer 2 FAILED: Consistency check failed"
        return 1
    fi

    verify_termination "$test_path" $n_processes 1

    log_success "Layer 2 PASSED"
    return 0
}

test_layer_3_tri() {
    log_info "========================================"
    log_info "Layer 3: Three-Process Standard Test"
    log_info "========================================"

    local test_path=$(setup_test_dir "layer3_tri")
    local n_processes=3

    create_hosts_file "$test_path" $n_processes
    create_config_file "$test_path" "1" "2" "3"

    log_info "Starting $n_processes processes with different proposals..."
    local pids=()
    for ((i=1; i<=n_processes; i++)); do
        local pid=$(start_process "$test_path" $i $TIMEOUT_BASIC)
        pids+=($pid)
    done

    wait_and_signal "${pids[@]}" 5

    log_info "Verifying outputs..."
    if ! check_output_exists "$test_path" $n_processes; then
        log_error "Layer 3 FAILED: Output files missing or empty"
        return 1
    fi

    local proposals=("1" "2" "3")
    if ! verify_validity "$test_path" $n_processes proposals; then
        log_error "Layer 3 FAILED: Validity check failed"
        return 1
    fi

    if ! verify_consistency "$test_path" $n_processes 1; then
        log_error "Layer 3 FAILED: Consistency check failed"
        return 1
    fi

    verify_termination "$test_path" $n_processes 1

    log_success "Layer 3 PASSED"
    return 0
}

test_layer_4_multi_slot() {
    log_info "========================================"
    log_info "Layer 4: Multi-Slot Test"
    log_info "========================================"

    local test_path=$(setup_test_dir "layer4_multi")
    local n_processes=3
    local n_proposals=5

    create_hosts_file "$test_path" $n_processes
    create_config_file "$test_path" "1 2" "2 3" "3 4" "4 5" "1 5"

    log_info "Starting $n_processes processes with $n_proposals proposals..."
    local pids=()
    for ((i=1; i<=n_processes; i++)); do
        local pid=$(start_process "$test_path" $i $TIMEOUT_BASIC)
        pids+=($pid)
    done

    wait_and_signal "${pids[@]}" 8

    log_info "Verifying outputs..."
    if ! check_output_exists "$test_path" $n_processes; then
        log_error "Layer 4 FAILED: Output files missing or empty"
        return 1
    fi

    local proposals=("1 2" "2 3" "3 4" "4 5" "1 5")
    if ! verify_validity "$test_path" $n_processes proposals; then
        log_error "Layer 4 FAILED: Validity check failed"
        return 1
    fi

    if ! verify_consistency "$test_path" $n_processes $n_proposals; then
        log_error "Layer 4 FAILED: Consistency check failed"
        return 1
    fi

    verify_termination "$test_path" $n_processes $n_proposals

    log_info "Checking output order..."
    for ((i=1; i<=n_processes; i++)); do
        local output_file="$test_path/output/${i}.output"
        local actual_lines=$(wc -l < "$output_file")
        if [ $actual_lines -ne $n_proposals ]; then
            log_warning "Process $i: Output may have incorrect number of lines"
        fi
    done

    log_success "Layer 4 PASSED"
    return 0
}

test_layer_5_stress() {
    log_info "========================================"
    log_info "Layer 5: Stress Test"
    log_info "========================================"

    local test_path=$(setup_test_dir "layer5_stress")
    local n_processes=5
    local n_proposals=50

    create_hosts_file "$test_path" $n_processes

    # Generate many proposals
    local proposals=()
    for ((i=1; i<=n_proposals; i++)); do
        local prop="$((i % 10 + 1)) $((i % 7 + 2)) $((i % 5 + 3))"
        proposals+=("$prop")
    done

    create_config_file "$test_path" "${proposals[@]}"

    log_info "Starting $n_processes processes with $n_proposals proposals..."
    local pids=()
    for ((i=1; i<=n_processes; i++)); do
        local pid=$(start_process "$test_path" $i $TIMEOUT_STRESS)
        pids+=($pid)
    done

    wait_and_signal "${pids[@]}" 30

    log_info "Verifying outputs..."
    if ! check_output_exists "$test_path" $n_processes; then
        log_error "Layer 5 FAILED: Output files missing or empty"
        return 1
    fi

    if ! verify_consistency "$test_path" $n_processes $n_proposals; then
        log_error "Layer 5 FAILED: Consistency check failed"
        return 1
    fi

    verify_termination "$test_path" $n_processes $n_proposals

    log_success "Layer 5 PASSED"
    return 0
}

# ==================== Main Execution ====================

main() {
    log_info "=========================================="
    log_info "Lattice Agreement Testing Suite"
    log_info "=========================================="

    # Check if binary exists
    if [ ! -f "$BIN_PATH" ]; then
        log_error "Binary not found: $BIN_PATH"
        log_info "Please run ./build.sh first"
        exit 1
    fi

    # Create test directory
    mkdir -p "$TEST_DIR"

    # Cleanup any existing processes
    cleanup

    local failed=0

    # Run enabled test layers
    if [ $RUN_LAYER_1_BASIC -eq 1 ]; then
        if ! test_layer_1_basic; then
            ((failed++))
        fi
        cleanup
        echo ""
    fi

    if [ $RUN_LAYER_2_DUAL -eq 1 ]; then
        if ! test_layer_2_dual; then
            ((failed++))
        fi
        cleanup
        echo ""
    fi

    if [ $RUN_LAYER_3_TRI -eq 1 ]; then
        if ! test_layer_3_tri; then
            ((failed++))
        fi
        cleanup
        echo ""
    fi

    if [ $RUN_LAYER_4_MULTI_SLOT -eq 1 ]; then
        if ! test_layer_4_multi_slot; then
            ((failed++))
        fi
        cleanup
        echo ""
    fi

    if [ $RUN_LAYER_5_STRESS -eq 1 ]; then
        if ! test_layer_5_stress; then
            ((failed++))
        fi
        cleanup
        echo ""
    fi

    # Final cleanup
    cleanup

    # Summary
    log_info "=========================================="
    if [ $failed -eq 0 ]; then
        log_success "All tests PASSED!"
        exit 0
    else
        log_error "$failed test(s) FAILED"
        exit 1
    fi
}

# Run main
main "$@"
