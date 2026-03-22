# CosmoFS v2 — Content-Addressed COW Filesystem

## Kern-Idee

Jeder Block ist sein eigener Hash. Writes sind Copy-on-Write.
Daraus fallen alle Features zwingend raus.

```
write("hello") → sha256 → Block 0x7a3f...
write("hello") → sha256 → selber Block, Refcount++
```

## Features (Konsequenzen aus COW + Content-Addressing)

### 1. Kein Journal

COW schreibt nie in-place. Neuer Baum wird atomar sichtbar durch
Root-Pointer-Update. Crash-Consistency ohne WAL — der alte Baum
ist noch da bis der neue vollstaendig geschrieben ist.

### 2. Snapshots in O(1)

Ein Snapshot ist ein Pointer auf die aktuelle Root-Node. Alte Bloecke
bleiben solange ein Snapshot sie referenziert. Kein rsync, kein
Kopieren. Jeder Checkpoint ist ein Pointer-Swap.

### 3. Dedup ist inharent

Zwei identische Dateien (oder identische Bloecke innerhalb
verschiedener Dateien) zeigen auf denselben physischen Block.
node_modules mit 50.000 Dateien ueber 3 Projekte? Einmal gespeichert.

### 4. Cloud-Sync als FS-Primitiv

Content-Addressing macht Sync trivial: "welche Hashes hat die
Gegenseite nicht?" Kein Diff auf Dateiebene, kein Timestamp-Vergleich.
Zwei CosmoOS-Rechner synchronisieren sich wie git-Repos.

```
Datei-Zustaende (im FS, nicht in einer App):
  ● lokal          Block ist auf SSD
  ◐ syncing        Upload/Download laeuft
  ○ remote-only    Nur Hash + Metadaten lokal, Inhalt in der Cloud
```

open() auf eine remote-only Datei fetcht transparent.
df zeigt lokalen UND Cloud-Speicher.

### 5. Strukturierte Metadaten

Typisierte Attribute, automatisch indiziert (BFS-Erbe, modern):

```c
// FS-native Query statt find+grep
cosmo_query("/home/Pictures", "date > 2025-01 AND width > 1920")
// → Ergebnis in Mikrosekunden, B+ Tree Index
```

Jede Datei hat ein Schema-freies Attribut-Set. Import-Tools schreiben
Metadaten beim Erstellen. Der FS indiziert automatisch.

### 6. SSD-Native

| HDD-Annahme              | SSD-Realitaet              | Konsequenz                    |
|---------------------------|----------------------------|-------------------------------|
| Seek ist teuer            | Random Read = Sequential   | Block-Gruppen irrelevant      |
| Schreiben ist guenstig    | Flash-Zellen verbrauchen   | COW + TRIM statt in-place     |
| Fragmentierung killt      | Fragmentierung egal        | Keine Defrag noetig           |
| Journal fuer Crash-Safety | FTL hat eigene Atomaritaet | COW reicht                    |

TRIM-Awareness: Nicht mehr referenzierte COW-Bloecke → sofort TRIM.
Flash-Zellen werden schneller recycelt.

## Architektur

```
CosmoFS v2
├── COW B+ Tree (kein Journal, crash-safe by design)
├── Content-Addressed Blocks (SHA-256, 4KB Chunks)
├── Inline Dedup (Refcount pro Block-Hash)
├── Snapshots (Root-Pointer-Liste, O(1) create/delete)
├── Typed Attributes + Auto-Index (B+ Tree pro Attribut)
├── Sync-Primitiv (Hash-Diff gegen Remote, transparenter Fetch)
└── TRIM-Pipeline (freed Blocks → async TRIM-Queue)
```

## Cloud-Backend: S3-kompatibel

Ein Protokoll, alle Backends. Content-Addressed Blocks ueber HTTP:

```
PUT /bucket/sha256hex    → Block hochladen
GET /bucket/sha256hex    → Block runterladen
HEAD /bucket/sha256hex   → Existiert der Block?
```

Key = Hash. Kein Konflikt, kein Ueberschreiben, idempotent.

### Kompatible Backends

| Backend       | Typ           | Anmerkung                          |
|---------------|---------------|-------------------------------------|
| MinIO         | Self-Hosted   | Open Source, S3-API, Homelab        |
| Garage        | Self-Hosted   | Rust, S3-API, verteiltes Homelab    |
| AWS S3        | Cloud         | Original                            |
| Cloudflare R2 | Cloud         | S3-kompatibel, kein Egress-Kosten   |
| Backblaze B2  | Cloud         | S3-kompatibel, guenstigster Storage |
| Azure Blob    | Cloud         | S3-Kompatibilitaetslayer            |
| GCS           | Cloud         | S3-Kompatibilitaetslayer            |

### Konfiguration

```
# ~/.config/cosmofs/sync.conf
endpoint = https://minio.local:9000
bucket = cosmo-blocks
access_key = ...
secret_key = ...
```

### Architektur-Trennung

```
Kernel (CosmoRT)          Userspace (CosmoPX)
  Inode-Zustand:            Sync-Daemon:
  ● lokal                   S3 PUT/GET/HEAD ueber HTTPS
  ◐ syncing                 Hash-Diff gegen Remote
  ○ remote-only             Transparenter Fetch bei open()
                            Background-Upload bei write()
```

Der Kernel kennt nur den Zustand im Inode (2 Bits). Der Sync-Daemon
in CosmoPX spricht S3 ueber HTTPS. Kein Cloud-Protokoll im Kernel.

Bei open() auf remote-only: Kernel blockiert, signalisiert Daemon
ueber eventfd, Daemon fetcht Block via S3 GET, schreibt in Block-Cache,
Kernel resumt den Prozess.

## Aufwand

Das bestehende CosmoFS (B+ Tree, Block-Cache) ist 80% der
Infrastruktur. COW ist ein neuer Schreibpfad, Content-Hashing
ist SHA-256 pro Block, Dedup ist eine Hash→Block-Tabelle.
Cloud-Sync ist Userspace (CosmoPX).

## Bezug zu CosmoFS v1

v1 (aktuell implementiert): B+ Tree Directories, WAL Journal,
Block-Cache LRU. Funktional, aber keine Alleinstellungsmerkmale
gegenueber ext4/BFS/XFS.

v2 ersetzt den Schreibpfad (in-place → COW), fuegt Content-Addressing
hinzu, und streicht das Journal. B+ Tree und Block-Cache bleiben.
