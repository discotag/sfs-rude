# Amiga SFS add-on для Haiku (пока read-only)

Первая модельная версия read-only filesystem add-on для Amiga Smart File System.
Реализация проверена на SFS v3, 512-байтовые блоки.

## Что уже работает

- Распознавание SFS и проверка checksum Root Block
- Чтение метки тома
- Монтирование раздела только для чтения
- Получение объектов через SFS Object Node Tree
- Чтение цепочек контейнеров объектов каталога
- `lookup` файлов и каталогов
- Чтение содержимого файлов через SFS extent B-tree
- Подсчёт свободных блоков по bitmap
- Перевод Amiga timestamp в Unix/Haiku time
- Преобразование имён через имеющийся charset-код: `latin1`, `cp1251`, `iso5`, `utf8`

Запись не реализована намеренно: устройство открывается с `O_RDONLY`, операции записи отсутствуют, `access()` запрещает запись.

## Пока не реализовано

- Запись на диск.
- Полная поддержка hard/soft links.
- Amiga comments и дополнительные атрибуты.
- Memory-mapped I/O / page-cache hooks.
- Проверка на всех вариантах и ревизиях SFS.

Тестировался SFS v3 с размером блока 512 байт. Идентификатор `SFS\2` тоже
принимается, если Root Block сообщает версию 3, но этот вариант отдельно не
проверялся.

## Сборка в дереве исходников Haiku

Скопируйте каталог `sfs` в:

```
haiku/src/add-ons/kernel/file_systems/sfs
```

Затем соберите Haiku обычным Jam build:

```
jam -q
```

Имя add-on: `sfs`. Для локальной установки:

```
/boot/home/config/non-packaged/add-ons/kernel/file_systems/sfs
```

или системной установки:

```
/boot/system/add-ons/kernel/file_systems/sfs
```

В этой среде нет Haiku SDK/toolchain, поэтому настоящий kernel add-on здесь
не собран и не загружался в ядро Haiku. Исходники прошли синтаксическую
проверку, а VFS callbacks были прогнаны через host-side compatibility harness
на реальном содержимом приложенного образа.

## Разметка диска

Add-on ожидает, что Haiku передаст ему **устройство самого SFS-раздела**, а не
весь диск. В `tiny.vdi` используется Amiga RDB partition table. Если Haiku не
показывает раздел, сначала нужно проверить наличие partitioning-system add-on
для Amiga RDB. Разбор внешней RDB-разметки не входит в этот filesystem add-on.


## Монтирование

После того как Haiku обнаружит RDB-раздел и add-on будет установлен:

```
mkdir -p /HaikuSFS
mount -t sfs /dev/disk/XXX/YYY /HaikuSFS
```

Замените `/dev/disk/XXX/YYY` фактическим устройством SFS-раздела,
которое показывает Haiku. Add-on в любом случае открывает его только для чтения.

- - -

# Amiga SFS add-on for Haiku (read-only yet)

The first model version of a read-only filesystem add-on for Amiga Smart File System.
The implementation has been tested with SFS v3 and 512-byte blocks.

## What’s working so far

- SFS detection and Root Block checksum verification
- Reading the volume label
- Mounting a partition as read-only
- Retrieving objects via the SFS Object Node Tree
- Reading directory object container chains
- `lookup` for files and directories
- Reading file contents via the SFS extent B-tree
- Counting free blocks using the bitmap
- Converting Amiga timestamps to Unix/Haiku time
- Converting names using the existing charset code: `latin1`, `cp1251`, `iso5`, `utf8`

Writing is intentionally not implemented: the device is opened with `O_RDONLY`, write operations are absent, and `access()` denies writing.

## Not yet implemented

- Writing to disk.
- Full support for hard/soft links.
- Amiga comments and additional attributes.
- Memory-mapped I/O / page-cache hooks.
- Testing across all SFS variants and revisions.

SFS v3 with a block size of 512 bytes was tested. The `SFS\2` identifier is also accepted if the Root Block reports version 3, but this variant has not been tested separately.

## Building in the Haiku source tree

Copy the `sfs` directory to:

```
haiku/src/add-ons/kernel/file_systems/sfs
```

Then build Haiku using the usual Jam build:

```
jam -q
```

Add-on name: `sfs`. For a local installation:

```
/boot/home/config/non-packaged/add-ons/kernel/file_systems/sfs
```

or for a system installation:

```
/boot/system/add-ons/kernel/file_systems/sfs
```

There is no Haiku SDK/toolchain in this environment, so the actual kernel add-on was not built here or loaded into the Haiku kernel. The source code passed syntax checks, and the VFS callbacks were exercised through a host-side compatibility harness using the actual contents of the attached image.

## Building in the Haiku source tree

Copy the `sfs` directory to:

```
haiku/src/add-ons/kernel/file_systems/sfs
```

Then build Haiku using the usual Jam build:

```
jam -q
```

The add-on name is `sfs`. For a local installation:

```
/boot/home/config/non-packaged/add-ons/kernel/file_systems/sfs
```

or for a system-wide installation:

```
/boot/system/add-ons/kernel/file_systems/sfs
```

There is no Haiku SDK/toolchain in this environment, so an actual kernel add-on
was not built here or loaded into the Haiku kernel. The source code passed a
syntax check, and the VFS callbacks were run through a host-side compatibility harness
using the actual contents of the attached image.

## Disk partitioning

The add-on expects Haiku to pass it **the SFS partition device itself**, not
the entire disk. `tiny.vdi` uses an Amiga RDB partition table. If Haiku does not show
the partition, first check whether an Amiga RDB partitioning-system add-on is
available. Parsing the external RDB partitioning is not part of this filesystem add-on.


## Mounting

Once Haiku detects the RDB partition and the add-on is installed:

```
mkdir -p /HaikuSFS
mount -t sfs /dev/disk/XXX/YYY /HaikuSFS
```

Replace `/dev/disk/XXX/YYY` with the actual SFS partition device
shown by Haiku. In any case, the add-on opens it read-only.