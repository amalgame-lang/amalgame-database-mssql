# NOTICE — amalgame-database-mssql

## Authorship

Copyright 2026 Bastien Mouget. Original work — see
`runtime/Amalgame_Database_MSSQL.h`.

Part of the Amalgame ecosystem
([github.com/amalgame-lang/Amalgame](https://github.com/amalgame-lang/Amalgame)).
External contributions are paused at the ecosystem level; see the
main repo's `CONTRIBUTING.md` for the policy.

AI tools (Anthropic Claude) were used during development. Per
the project's authorship policy, AI is treated as a tool, not a
co-author at law.

## Licence

Apache License 2.0. See `LICENSE` for the full text.

## Third-party content

**None vendored.** This package binds to the unixODBC driver
manager (`libodbc`) plus the *Microsoft ODBC Driver for SQL
Server*. Both are provided by the user's operating system at
compile time (via `unixodbc-dev` / `unixODBC-devel` and the
MS-distributed `msodbcsql18` package) and at run time (via the
matching shared libraries).

[unixODBC](https://www.unixodbc.org/) is distributed under the
[GNU LGPL 2.1](https://github.com/lurcher/unixODBC/blob/master/COPYING)
— dynamic-linking against an LGPL library is explicitly permitted
for Apache-2.0 consumers (no copyleft virality through dynamic
linking; the LGPL §5 / §6 exception covers this exact case).

The *Microsoft ODBC Driver for SQL Server* is distributed by
Microsoft under their own
[redistribution licence](https://learn.microsoft.com/sql/connect/odbc/linux-mac/installing-the-microsoft-odbc-driver-for-sql-server)
— users must download and install it themselves; this package
neither bundles nor redistributes it.

This package does not include or redistribute any unixODBC or
Microsoft driver code; users obtain those independently from
their OS package manager or directly from Microsoft.

## Trademarks

"Microsoft", "SQL Server", and "Azure SQL" are trademarks of
Microsoft Corporation. "ODBC" is a Microsoft-coined term. This
repository uses those names solely to identify the database
engines the package binds to. No trademark claim is asserted.
