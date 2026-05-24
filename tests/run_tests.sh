#!/bin/bash
# ─────────────────────────────────────────────────────
#  amalgame-database-mssql — Test Runner
#  Usage: ./tests/run_tests.sh [/path/to/amc]
#
#  Double-gated:
#    1. unixodbc-dev (sql.h) is installed (header probe via gcc -E)
#    2. A SQL Server (or Azure SQL) is reachable via the
#       MSSQL_CONNSTR env var. Default exported by this script:
#         Driver={ODBC Driver 18 for SQL Server};Server=127.0.0.1,1433;
#         Database=master;UID=sa;PWD=YourStrong!Passw0rd;
#         TrustServerCertificate=yes
#
#  Start a server locally:
#    docker run --rm -d --name mssqltest -p 1433:1433 \
#      -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD='YourStrong!Passw0rd' \
#      mcr.microsoft.com/mssql/server:2022-latest
# ─────────────────────────────────────────────────────

set -u

if [ $# -ge 1 ]; then
    AMC="$1"
elif [ -n "${AMC:-}" ]; then
    :
elif command -v amc >/dev/null 2>&1; then
    AMC="$(command -v amc)"
else
    echo "ERROR: amc not found." >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_RUNTIME="$PKG_ROOT/runtime"

AMC_DIR="$(cd "$(dirname "$AMC")" && pwd)"
if [ -d "$AMC_DIR/runtime" ]; then
    AMC_RUNTIME="$AMC_DIR/runtime"
elif [ -d "$AMC_DIR/../share/amalgame/runtime" ]; then
    AMC_RUNTIME="$AMC_DIR/../share/amalgame/runtime"
elif [ -n "${AMC_RUNTIME:-}" ]; then
    :
else
    echo "ERROR: amc runtime/ not found. Set AMC_RUNTIME=..." >&2
    exit 2
fi

BUILD_DIR="$(mktemp -d -t amssql-XXXXXX)"
trap 'rm -rf "$BUILD_DIR"' EXIT
PROJ_DIR="$BUILD_DIR/proj"
mkdir -p "$PROJ_DIR"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

echo ""
echo "════════════════════════════════════════════"
echo "  amalgame-database-mssql — Tests"
echo "════════════════════════════════════════════"
echo "  amc:     $AMC ($("$AMC" --version 2>&1 | head -1))"
echo "  runtime: $AMC_RUNTIME"
echo ""

# ── Gate 1 : unixODBC header present? ─────────────
echo "── Probing unixODBC (sql.h) ──────────────"
ODBC_OK=0
for h in 'sql.h' 'unixodbc/sql.h'; do
    echo "#include <$h>" > "$BUILD_DIR/_inc.c"
    if gcc -E "$BUILD_DIR/_inc.c" >/dev/null 2>&1; then
        ODBC_OK=1
        echo "  $h:                  found"
        break
    fi
done
if [ "$ODBC_OK" = "0" ]; then
    echo "  sql.h:                  NOT FOUND (install unixodbc-dev / unixODBC-devel)"
fi
echo ""

# ── Gate 2 : MSSQL_CONNSTR set + server reachable ─
: "${MSSQL_CONNSTR:=}"
if [ -z "$MSSQL_CONNSTR" ]; then
    MSSQL_CONNSTR="Driver={ODBC Driver 18 for SQL Server};Server=127.0.0.1,1433;Database=master;UID=sa;PWD=YourStrong!Passw0rd;TrustServerCertificate=yes"
fi
export MSSQL_CONNSTR

MSSQL_HOST="${MSSQL_HOST:-127.0.0.1}"
MSSQL_PORT="${MSSQL_PORT:-1433}"
DB_OK=0
if (echo > /dev/tcp/$MSSQL_HOST/$MSSQL_PORT) 2>/dev/null; then
    DB_OK=1
    echo "  mssql:                  reachable at $MSSQL_HOST:$MSSQL_PORT"
else
    echo "  mssql:                  NOT reachable at $MSSQL_HOST:$MSSQL_PORT"
fi
echo ""

# ── Stage fake cache for the test fixture ────────────
FAKE_CACHE="$BUILD_DIR/cache"
PKG_GIT="github.com/amalgame-lang/amalgame-database-mssql"
PKG_TAG="${PKG_TAG:-v0.1.0}"
FAKE_SHA="deadbeefcafebabe0000000000000000000000ab"
SHORT_SHA="${FAKE_SHA:0:8}"
PKG_CACHE_DIR="$FAKE_CACHE/$PKG_GIT/${PKG_TAG}_${SHORT_SHA}"

mkdir -p "$(dirname "$PKG_CACHE_DIR")"
rm -rf "$PKG_CACHE_DIR"
ln -s "$PKG_ROOT" "$PKG_CACHE_DIR"

cat > "$PROJ_DIR/amalgame.lock" <<EOF
[[package]]
name = "amalgame-database-mssql"
git  = "$PKG_GIT"
tag  = "$PKG_TAG"
rev  = "$FAKE_SHA"
EOF

export AMALGAME_PACKAGES_DIR="$FAKE_CACHE"

run_test() {
    local name="$1"
    local expected="$2"
    printf "  %-38s" "$name"
    if [ "$ODBC_OK" = "0" ]; then
        echo -e "${YELLOW}SKIP${NC} (unixodbc-dev not installed)"
        SKIP=$((SKIP + 1)); return
    fi
    if [ "$DB_OK" = "0" ]; then
        echo -e "${YELLOW}SKIP${NC} (no mssql at $MSSQL_HOST:$MSSQL_PORT)"
        SKIP=$((SKIP + 1)); return
    fi
    cp "$SCRIPT_DIR/stdlib_mssql.am" "$PROJ_DIR/test.am"
    local out_base="$PROJ_DIR/test"
    local out
    out=$(cd "$PROJ_DIR" && "$AMC" -o test test.am 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (amc)"; echo "$out" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL + 1)); return
    fi
    if [ ! -f "$out_base.c" ]; then
        echo -e "${RED}FAIL${NC} (no .c)"; FAIL=$((FAIL + 1)); return
    fi
    gcc -O2 -w \
        -I"$AMC_RUNTIME" -I"$PKG_RUNTIME" \
        "$out_base.c" \
        -lgc -lm -ldl -lpthread -lodbc \
        -o "$out_base" 2>"$BUILD_DIR/link.log"
    if [ ! -x "$out_base" ]; then
        echo -e "${RED}FAIL${NC} (gcc link)"
        cat "$BUILD_DIR/link.log" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL + 1)); return
    fi
    local run_output
    run_output=$("$out_base" 2>&1)
    if echo "$run_output" | grep -qF "$expected"; then
        echo -e "${GREEN}PASS${NC}"; PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "    expected: $expected"
        echo "    got:      $(echo "$run_output" | head -3 | tr '\n' '|')"
        FAIL=$((FAIL + 1))
    fi
}

echo "── Microsoft SQL Server ────────────────────"
run_test "open"                   "[PASS] open"
run_test "ServerVersion"          "[PASS] ServerVersion"
run_test "CREATE TABLE"           "[PASS] CREATE TABLE"
run_test "INSERT Changes"         "[PASS] INSERT Changes==1"
run_test "SELECT 3 rows"          "[PASS] SELECT 3 rows"
run_test "SELECT cell content"    "[PASS] SELECT cell content"
run_test "IDENTITY id starts at 1" "[PASS] IDENTITY id starts at 1"
run_test "UPDATE Changes==2"      "[PASS] UPDATE Changes==2"
run_test "bad SQL reports error"  "[PASS] bad SQL reports error"
run_test "Close drops connection" "[PASS] Close drops connection"

echo ""
echo "────────────────────────────────────────────"
echo -e "  ${GREEN}PASS: $PASS${NC}  |  ${RED}FAIL: $FAIL${NC}  |  ${YELLOW}SKIP: $SKIP${NC}"
echo "────────────────────────────────────────────"
echo ""

[ $FAIL -eq 0 ] && exit 0 || exit 1
