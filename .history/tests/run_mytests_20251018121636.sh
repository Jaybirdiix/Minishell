#!/usr/bin/env bash
# Simple local test runner for your student shell "Bash".
# Run this from inside your starter-code directory.
# It will:
#   1) Ensure ./Bash exists (build minishell.c if present)
#   2) Generate mytests/*.in/.out using /bin/bash (or REF)
#   3) Run ./Bash on each .in and compare to expected .out
#
# Usage:
#   chmod +x run_mytests.sh
#   ./run_mytests.sh
#
# Variables you can override:
#   REF=/bin/bash        # reference shell to generate expected output
#   TESTS_FILE=test_cases.txt
#   MYTESTS_DIR=mytests
#   ACTUAL_DIR=actual

set -Eeuo pipefail

REF="${REF:-/bin/bash}"
TESTS_FILE="${TESTS_FILE:-test_cases.txt}"
MYTESTS_DIR="${MYTESTS_DIR:-mytests}"
ACTUAL_DIR="${ACTUAL_DIR:-actual}"
STU="${STU:-./Bash}"   # <— add this

# 1) Ensure we're in the directory containing this script.
cd "$(dirname "$0")"

# Ensure student shell exists (build if obvious)
if [[ ! -x "$STU" ]]; then
  if [[ -f ../src/minishell.c && "$STU" == "../src/minishell" ]]; then
    echo "[build] gcc -std=c11 -Wall -Wextra -O2 -o $STU ../src/minishell.c"
    gcc -std=c11 -Wall -Wextra -O2 -o "$STU" ../src/minishell.c
  else
    echo "[warn] student shell not found: $STU"
    exit 2
  fi
fi


# 3) Generate expected outputs using the reference shell
echo "[gen] Generating expected outputs with REF=$REF"
python3 ./turn_to_tests_ref.py

# 4) Run tests
mkdir -p "$ACTUAL_DIR"
pass=0
total=0
for inpath in "$MYTESTS_DIR"/mytests*.in; do
  [[ -e "$inpath" ]] || { echo "[info] no tests found in $MYTESTS_DIR"; break; }
  base="$(basename "$inpath" .in)"
  exp="$MYTESTS_DIR/$base.out"
  act="$ACTUAL_DIR/$base.out"
  ((total++)) || true

  # Run student shell and capture both stdout+stderr
  # ./Bash < "$inpath" > "$act" 2>&1 || true
  "$STU" < "$inpath" > "$act" 2>&1 || true

  if diff -u "$exp" "$act" > /dev/null; then
    echo "[PASSED] $base"
    ((pass++)) || true
  else
    echo "[FAILED] $base"
    echo "Test Command: $(tr -d '\n' < "$inpath")"
    echo "Expected Output:"
    sed -e 's/^/  /' "$exp"
    echo "Actual Output:"
    sed -e 's/^/  /' "$act"
    echo
  fi
done

echo "Tests completed: $pass/$total tests passed."
