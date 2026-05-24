# amalgame-database-mssql

Microsoft SQL Server binding for
[Amalgame](https://github.com/amalgame-lang/Amalgame). Dynamic-
linked to **unixODBC** plus the vendor-distributed
**Microsoft ODBC Driver for SQL Server** — no vendored library.
Sibling of
[`amalgame-database-postgresql`](https://github.com/amalgame-lang/amalgame-database-postgresql)
and
[`amalgame-database-mysql`](https://github.com/amalgame-lang/amalgame-database-mysql);
same dynamic-link manifest pattern.

Works against on-prem **SQL Server 2017+**, **Azure SQL
Database**, and **Azure SQL Managed Instance**. TDS protocol
fully handled inside the ODBC driver.

## Prerequisites

Two packages are required on the build machine: the unixODBC
driver manager and Microsoft's ODBC driver for SQL Server.

### unixODBC driver manager

| OS / distro | Command |
|---|---|
| Debian / Ubuntu | `sudo apt install unixodbc-dev` |
| Fedora / RHEL / Rocky | `sudo dnf install unixODBC-devel` |
| Arch / Manjaro | `sudo pacman -S unixodbc` |
| Alpine | `apk add unixodbc-dev` |
| macOS (Homebrew) | `brew install unixodbc` |
| Windows | ODBC ships with the OS (`odbc32.dll`) — link with `-lodbc32` instead of `-lodbc` |

### Microsoft ODBC Driver for SQL Server

Follow Microsoft's per-OS instructions at:
<https://learn.microsoft.com/sql/connect/odbc/linux-mac/installing-the-microsoft-odbc-driver-for-sql-server>

Short version for Debian/Ubuntu:

```bash
curl -s https://packages.microsoft.com/keys/microsoft.asc | sudo apt-key add -
curl -s https://packages.microsoft.com/config/ubuntu/$(lsb_release -rs)/prod.list \
    | sudo tee /etc/apt/sources.list.d/mssql-release.list
sudo apt update
sudo ACCEPT_EULA=Y apt install msodbcsql18
```

macOS (Homebrew):

```bash
brew tap microsoft/mssql-release
brew install msodbcsql18
```

On the **deploy** machine you only need the runtime variants
(`unixodbc` + `msodbcsql18`), not the `-dev` headers.

## Install

```bash
amc package add mssql                                              # via index
amc package add github.com/amalgame-lang/amalgame-database-mssql@v0.2.0
```

Requires **amc 0.8.40+** (for `returns_generic` on QueryAll).

## Surface

```amalgame
import Amalgame.Database.MSSQL

let connStr: string = "Driver={ODBC Driver 18 for SQL Server};"
    + "Server=127.0.0.1,1433;Database=master;UID=sa;"
    + "PWD=YourStrong!Passw0rd;TrustServerCertificate=yes"

let db = MSSQL.Open(connStr)
if (!MSSQL.IsOpen(db)) {
    Console.WriteLine("connect failed: " + MSSQL.LastError(db))
    return
}

MSSQL.Exec(db, "IF OBJECT_ID('notes','U') IS NULL "
    + "CREATE TABLE notes (id INT IDENTITY(1,1) PRIMARY KEY, body NVARCHAR(200))")
MSSQL.Exec(db, "INSERT INTO notes (body) VALUES ('hello sql server')")

let rows = MSSQL.QueryAll(db, "SELECT id, body FROM notes ORDER BY id")
let i: int = 0
while (i < rows.Count()) {
    let row = rows.Get(i)
    Console.WriteLine(row.Get(0) + ": " + row.Get(1))
    i = i + 1
}

MSSQL.Close(db)
```

### v0.1.0 method surface

| Method | Returns | Notes |
|---|---|---|
| `MSSQL.Open(connStr)` | `AmalgameMSSQL*` | ODBC keyword=value connection string |
| `MSSQL.Close(db)` | `void` | Idempotent; GC also closes leaked handles |
| `MSSQL.IsOpen(db)` | `bool` | Live connection check |
| `MSSQL.LastError(db)` | `string` | Empty on success; concatenates ODBC diagnostic chain |
| `MSSQL.Exec(db, sql)` | `bool` | DDL / INSERT / UPDATE / DELETE; updates Changes() |
| `MSSQL.QueryAll(db, sql)` | `List<List<string>>` | SELECT, text mode (SQL_C_CHAR) |
| `MSSQL.Changes(db)` | `int` | Rows affected by last Exec / row count of last QueryAll |
| `MSSQL.ServerVersion(db)` | `string` | "16.00.4115" (2022), "15.00.4153" (2019), … |

### Connection string

Standard ODBC keyword=value syntax. Common shapes:

```text
# Local Docker MSSQL with self-signed TLS
Driver={ODBC Driver 18 for SQL Server};Server=127.0.0.1,1433;Database=master;
UID=sa;PWD=YourStrong!Passw0rd;TrustServerCertificate=yes

# Azure SQL with username/password
Driver={ODBC Driver 18 for SQL Server};Server=tcp:my.database.windows.net,1433;
Database=mydb;UID=app@my;PWD=...;Encrypt=yes

# Azure SQL with Managed Identity / AAD Integrated
Driver={ODBC Driver 18 for SQL Server};Server=tcp:my.database.windows.net,1433;
Database=mydb;Authentication=ActiveDirectoryIntegrated
```

The driver name in `Driver={...}` must match what's installed on
the host. `ODBC Driver 18 for SQL Server` is the current
recommended version; `ODBC Driver 17` works too if 18 isn't
available. Run `odbcinst -q -d` to list installed drivers.

## Pixel layout / data model

The v1 surface stringifies every cell via
`SQLGetData(SQL_C_CHAR)` — the ODBC driver does the type
conversion (ints, decimals, dates, GUIDs all become readable
text). NULL cells materialise as `""` — callers that need to
distinguish `NULL` from `''` should use parameter binding +
typed accessors in v2.

Cells longer than 255 bytes (the initial probe buffer) are
re-fetched into a right-sized buffer so long `NVARCHAR(MAX)`
payloads round-trip cleanly.

## Deferred to v2

- `SQLBindParameter` parameter binding (`?` placeholders)
- `SQLPrepare` + repeated `SQLExecute` statement reuse
- Typed column accessors (`AsInt(col)`, `AsBytes(col)`, `AsDateTime(col)`)
- `SQLBulkOperations` / `bcp_*` bulk insert
- Async query mode (`SQL_ATTR_ASYNC_ENABLE`)
- Table-valued parameters (TVPs)
- First-class AAD / managed-identity helpers (works today via
  the connection-string `Authentication=ActiveDirectory…` keyword
  but a dedicated AM API would be cleaner)

## Threading

`AmalgameMSSQL*` is single-owner. ODBC handles are not safely
sharable across threads under load — use distinct handles per
thread. Async / pipelined query lands in v2.

## Tests

```bash
./tests/run_tests.sh /path/to/amc
```

Double-gated runner. Both `unixodbc-dev` AND a reachable
SQL Server on `127.0.0.1:1433` must be present, else every
case SKIPs cleanly. Start a server locally with:

```bash
docker run --rm -d --name mssqltest -p 1433:1433 \
  -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD='YourStrong!Passw0rd' \
  mcr.microsoft.com/mssql/server:2022-latest
```

Then optionally export a custom connection string:

```bash
export MSSQL_CONNSTR='Driver={ODBC Driver 18 for SQL Server};...'
```

(The runner falls back to a sensible default matching the docker
command above.)

## Licence

Apache-2.0 — see [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).
unixODBC itself is LGPL-2.1; dynamic-linking against an LGPL
library is explicitly permitted for Apache-2.0 consumers (no
copyleft virality through dynamic linking).
