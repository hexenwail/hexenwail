#!/bin/bash
# Simple test script for mathlib-rs
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
crate="$(cd "$here/.." && pwd)"
crate="$(cd "$here/.." && pwd)"

echo "Building mathlib-rs..."
cargo build --release --manifest-path "$crate/Cargo.toml"

echo "Linking test program..."
# Create a simple test that links against the Rust staticlib
cat > "$here/test_mathlib.c" << 'EOF'
#include <stdio.h>
#include "q_stdinc.h"
#include "model.h"
#include "mathlib.h"

int main() {
    printf("=== Mathlib-RS Link Test ===\n");
    
    /* Test that we can call the functions */
    int result = Q_isnan(0.0f);
    printf("Q_isnan(0.0) = %d\n", result);
    
    float angle = anglemod(45.0f);
    printf("anglemod(45.0) = %f\n", angle);
    
    int gcd = GreatestCommonDivisor(48, 18);
    printf("GCD(48, 18) = %d\n", gcd);
    
    printf("=== Link test completed ===\n");
    return 0;
}
EOF

# Compile and link test
gcc -I"$here/../.." -I"$here/../.."/common -I"$here/../.."/engine/h2shared -L"$here/target/release" -lmathlib_rs "$here/test_mathlib.c" -o "$here/test_mathlib" -lm

echo "Running test..."
"$here/test_mathlib"

echo "Cleaning up..."
rm -f "$here/test_mathlib.c" "$here/test_mathlib"