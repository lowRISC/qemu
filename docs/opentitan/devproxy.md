## DevProxy Protocol

### Header

#### DevProxy header

```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|            COMMAND            |            LENGTH             |
+---------------+---------------+---------------+---------------+
|                              UID                            |I|
+---------------+---------------+---------------+---------------+
```

#### DOE PCIe header [doe-pcie-header]

```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             VendorID          |ObjectDataType |       -       |
+---------------+---------------+---------------+---------------+
|             Length                |            -              |
+---------------+---------------+---------------+---------------+
```

#### Length fields

##### Notes
 * Values are encoded using the LE (litle endian) format
 * `LENGTH` is the length in bytes excluding the first 8 bytes of the MBXPROXY protocol header,
   coded on 16 bits. Payload is therefore < 64KB.
 * This field is different from the `Length` is the length in 32-bit words (DW per Intel
   terminology) of the Data Object itself, including the 2 first 32-bit words of the DOE header,
   coded on 18 bits. Theoritical DOE payload is therefore ~1MB, which is capped by the MBX protocol.
   It is not expected to have DOE packets larger than 4KB anyway.

#### UID and Initiator fields

DevProxy header is not related whatsoever to the content of the DOE packet, it is a simplistic TLV
protocol, extended with a 31+1 bit UID. This UID is not accounted for in the `LENGTH` field, and is
always present. The UID is a monotonic increasing integer. The initiator of a request should
increment the UID for each request. The receiver should reply once with the same UID. The receiver
should always acknowledge a request, whether it is successfully handled or not, in which case it
should reply with a error code, using the UID of the failed command. The MSB of the 32-bit field
that contains the UID is used to distinguish the peer. The QEMU peer is using `1`, the remote peer
should use `0`. There are therefore two independent UID sequences: each UID is managed and
incremented by the initiator, the receiver never modifies the UID. UIDs are used as sanity check to
ensure sync is not lost between requests and responses. It is a fatal error to miss one UID or to
reuse one. Roll over cases are not managed (2G requests). DevProxy protocol can be used with any
kind of payload, i.e. not DOE payloads. This is the case for the first requests performed on the
communication link for example (see below).

#### Command fields

Commands are coded with a 16-bit field. It is expected to only use ASCII bytes for commands, i.e.
two characters that should be enough to encode many commands. Moreover, the initiator of a request
should use UPPERCASE command, and the receiver should reply with the same command using only
lowercased character. This allows to disambiguate initiators from responders, and use of ASCII
character helps tracking and debugging packets. If a command cannot be executed on the receiver
side, the receiver should reply with a `xx` response command (lowercased). The UID enables to track
which command has failed. There is no other cases where the initiator command and the response
command differ (case-insensitive). `XX` is therefore a reserved command.

#### Role fields

`Role` field encode an access control list role to use to access a register or a memory location.
Valid values depends on the remote proxy, however the highest (role = 0xF) is reserved to specify
that role field should be ignored, and role-less accessor should be used by the remote proxy.

### Request/Response packets

#### Error

##### Request

Not applicable

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'xx'              |            4..4+N             |
+---------------+---------------+---------------+---------------+
|                              UID                            |S|
+---------------+---------------+---------------+---------------+
|            Address            |         Device        |   -   |
+---------------+---------------+---------------+---------------+
|                          Error Code                           |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                         (Error Message)                       |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
```
* `Error Code` is a 32-bit error code that is always present

```
+------------+---------------+----------------------------------+
| Error Code | Error Type    | Error Description                |
+------------+---------------+----------------------------------+
|    0x0     |               | No error                         |
+------------+---------------+----------------------------------+
|    0x1     | Undefined     | Unknown                          |
+------------+---------------+----------------------------------+
|    0x101   | Request       | Invalid command length           |
+------------+---------------+----------------------------------+
|    0x102   | Request       | Invalid command code             |
+------------+---------------+----------------------------------+
|    0x103   | Request       | Invalid request identifier       |
+------------+---------------+----------------------------------+
|    0x104   | Request       | Invalid specifier identifier     |
+------------+---------------+----------------------------------+
|    0x105   | Request       | Invalid device identifier        |
+------------+---------------+----------------------------------+
|    0x106   | Request       | Invalid request                  |
+------------+---------------+----------------------------------+
|    0x107   | Request       | Invalid address/register address |
+------------+---------------+----------------------------------+
|    0x201   | State         | Device in error                  |
+------------+---------------+----------------------------------+
|    0x401   | Local         | Cannot read device               |
+------------+---------------+----------------------------------+
|    0x402   | Local         | Cannot write device              |
+------------+---------------+----------------------------------+
|    0x403   | Local         | Truncated response               |
+------------+---------------+----------------------------------+
|    0x404   | Local         | Incomplete write                 |
+------------+---------------+----------------------------------+
|    0x405   | Local         | Out of resources                 |
+------------+---------------+----------------------------------+
|    0x801   | Internal      | Unsupported device               |
+------------+---------------+----------------------------------+
|    0x802   | Internal      | Duplicated unique identifier     |
+------------+---------------+----------------------------------+
```

* `Error Message` is an optional error message to help diagnose the actual error. The length of the
  error message string can be retrieved from the `LENGTH` field.

Any request may be responded with an error packet.

#### Handshake

Handshake is the first command exchanged over the communication link. It enables sanity check of the
link, and to retrieve the version of the protocol the QEMU peer implement. It is the application
responsability to ensure it can successfully communicate with the QEMU proxy.

Only initiated by the application.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'HS'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'hs'              |               4               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
| Version Min   | Version Maj   |                               |
+---------------+---------------+---------------+---------------+
```

The current version for this documentation is v0.15.

Note that semantic versionning does not apply for v0 series.

#### Logmask

Logmask can be used to change the qemu_log_mask bitmap at runtime, so log
settings can be altered for specific runtime ranges, for a specific test for
example

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'HL'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---+-----------+---------------+---------------+---------------+
| Op|                         Log mask                          |
+---+-----------+---------------+---------------+---------------+
```

* `Op`: Log operation, among:
  * `0`: change nothing, only read back the current log levels
  * `1`: add new log channels from the log mask
  * `2`: clear log channels from the log mask
  * `3`: apply the log mask as is, overridding previous log channel settings

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'hl'              |               4               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---+-----------+---------------+---------------+---------------+
| 0 |                    Previous log mask                      |
+---+-----------+---------------+---------------+---------------+
```

#### Enumerate Devices [enumerate-devices]

Enumerate should be called by the Application to retrieve the list of remote devices that can be
driven from the Application over the communication link.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'ED'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'ed'              |            0..4*7N            |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|            Offset             |         Device        |   -   |
+---------------+---------------+---------------+---------------+
|                          Base Address                         |
+---------------+---------------+---------------+---------------+
|                           Word Count                          |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
|            Offset             |         Device        |   -   |
+---------------+---------------+---------------+---------------+
|                          Base Address                         |
+---------------+---------------+---------------+---------------+
|                           Word Count                          |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
|                             ....                              |
+---------------+---------------+---------------+---------------+
|            Offset             |         Device        |   -   |
+---------------+---------------+---------------+---------------+
|                          Base Address                         |
+---------------+---------------+---------------+---------------+
|                           Word Count                          |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
```
Reponse contains 0 up to N devices, each device is described with a 28-byte entry, where:

* `Device` is a unique device identifier that should be used to select the device for all further
   requests to the device.
* `Offset` is the relative base address of the first register than can be accessed on the device,
   expressed in 32-bit word count
* `RegCount` is the count of 32-bit registers that can be accessed, starting from the base address,
   only if b0 is zero.
* `WordCount` is the count of 32-bit slots for a memory-type device
* `Base Address` is the base address of the device in the address space as seen from the local CPU.
* `Identifier` is an arbitrary 16-character string that describes the device.

The count of device entries can be retrieved from the `LENGTH` field.

#### Enumerate Memory Spaces [enumerate-mr-spaces]

Enumerate should be called by the Application to retrieve the list of remote memory spaces that can
be used from the Application over the communication link.

Each memory space is a top-level memory region, _i.e._ a root memory region.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'ES'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'es'              |            0..11*4N           |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                             -                 |      MSpc     |
+---------------+---------------+---------------+---------------+
|                         Start Address                         |
+---------------+---------------+---------------+---------------+
|                             Size                              |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
|                             -                 |      MSpc     |
+---------------+---------------+---------------+---------------+
|                         Start Address                         |
+---------------+---------------+---------------+---------------+
|                             Size                              |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
|                             ....                              |
+---------------+---------------+---------------+---------------+
|                             -                 |      MSpc     |
+---------------+---------------+---------------+---------------+
|                         Start Address                         |
+---------------+---------------+---------------+---------------+
|                             Size                              |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
```
Reponse contains 0 up to N devices, each device is described with a 44-byte entry, where:

* `MSpc` is a unique address space identifier than can be used as a selector in other proxy
  commands.
* `Start Address` is the lowest valid address in the address space.
* `Size` is the size (in bytes) of the address space
* `Identifier` is an arbitrary 32-character string that describes the memory space.

The count of address spaces can be retrieved from the `LENGTH` field.

#### Register Read

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'RW'              |               8               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|            Address            |         Device        | Role  |
+---------------+---------------+---------------+---------------+
```

Read the content of a 32-bit word register, where

* `Address` is the register index, i.e. the relative address in bytes / 4.
* `Role` is the initiator role to use to access the device
* `Device` is the device to access (see [Enumerate](#enumerate-devices))

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'rw'              |               4               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                             Value                             |
+---------------+---------------+---------------+---------------+
```

#### Register Write

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'WW'              |              12               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|            Address            |         Device        | Role  |
+---------------+---------------+---------------+---------------+
|                             Value                             |
+---------------+---------------+---------------+---------------+
|                             Mask                              |
+---------------+---------------+---------------+---------------+
```

Write the content of a 32-bit word register, where

* `Address` is the register index, i.e. the relative address in bytes / 4.
* `Role` is the initiator role to use to access the device
* `Device` is the device to access (see [Enumerate](#enumerate-devices))
* `Value` is the (optionally masked) 32-bit value to write
* `Mask` is the mask of bits to be written. Masked bits (0) are left unmodified, unmasked bits (1)
  are replaced with the bits from the value field. The remote device use read-modify-write sequence
  if the destination device does not support atomic set.

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'ww'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

#### Read Buffer

Read the content of several subsequence 32-bit registers, where

* `Address` is the index of the first register, i.e. the relative address in bytes / 4. The address
  is automatically incremented by the receiver side to read each register
* `Role` is the initiator role to use to access the device
* `Device` is the device to access (see [Enumerate](#enumerate-devices))
* `Count` is the number of the register to read.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'RS'              |               8               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|            Address            |         Device        | Role  |
+---------------+---------------+---------------+---------------+
|                             Count                             |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'rs'              |           4..4*Count          |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                             Value #0                          |
+---------------+---------------+---------------+---------------+
|                             Value #1                          |
+---------------+---------------+---------------+---------------+
|                              ....                             |
+---------------+---------------+---------------+---------------+
|                             Value #N-1                        |
+---------------+---------------+---------------+---------------+
```

#### Write Buffer

Write the content of several subsequence 32-bit registers, where

* `Address` is the index of the first register, i.e. the relative address in bytes / 4. The address
  is automatically incremented by the receiver side to write each register
* `Role` is the initiator role to use to access the device
* `Device` is the device to access (see [Enumerate](#enumerate-devices))
* `Value` are the values to write in each register.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'WS'              |           4 + 4*Count         |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|            Address            |         Device        | Role  |
+---------------+---------------+---------------+---------------+
|                             Value #0                          |
+---------------+---------------+---------------+---------------+
|                             Value #1                          |
+---------------+---------------+---------------+---------------+
|                              ....                             |
+---------------+---------------+---------------+---------------+
|                             Value #N-1                        |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'ws'              |               4               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                             Count                             |
+---------------+---------------+---------------+---------------+
```

#### Read Mailbox

Read the content of a DOE mailbox, where

* `Address` is the index of the mailbox register, i.e. the relative address in bytes / 4.
* `Role` is the initiator role to use to access the device
* `Device` is the device to access (see [Enumerate](#enumerate-devices))
* `Count` is the number of the 32-bit words to read out of the mailbox

The content of the mailbox is read till either `Count` 32-bit words are read _or_ the Data Object
has been fully read. Each read word is automatically flagged as read in the remote mailbox (see DOE
protocol for detail).

It is the responsability of the Application to ensure that the remote mailbox is ready to be read
before initiating this command. If an error occurs during the read out of the mailbox, an error
response should be returned. It is not an error to read less words than requests, _i.e._ it is safe
to specify a high count of words to read - as long as the response may fit into a DevProxy packet.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'RX'              |               8               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|            Address            |         Device        | Role  |
+---------------+---------------+---------------+---------------+
|                             Count                             |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'rx'              |            4..4*Count         |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                             Value #0                          |
+---------------+---------------+---------------+---------------+
|                             Value #1                          |
+---------------+---------------+---------------+---------------+
|                              ....                             |
+---------------+---------------+---------------+---------------+
|                             Value #Count-1                    |
+---------------+---------------+---------------+---------------+
```

#### Write Mailbox

Write a message into the DOE mailbox, where

* `Address` is the index of the mailbox register, i.e. the relative address in bytes / 4.
* `Role` is the initiator role to use to access the device
* `Device` is the device to access (see [Enumerate](#enumerate-devices))
* `Value` are the values to write to the mailbox

It is the responsability of the Application to ensure that the remote mailbox is reday to be written
before initiating this command. If an error occurs during the write to the mailbox, an error
response should be returned.

The responder side automatically triggers the GO bit once all values have been written to the
destination mailbox (if no error occurs).

The Application is in charge of formatting the payload to ensure that it contains a valid DOE object
(including formatting the header, see [DOE PCIe header](#doe-pcie-header))

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'WX'              |            4 + 4*Count        |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|            Address            |         Device        | Role  |
+---------------+---------------+---------------+---------------+
|                             Value #0                          |
+---------------+---------------+---------------+---------------+
|                             Value #1                          |
+---------------+---------------+---------------+---------------+
|                              ....                             |
+---------------+---------------+---------------+---------------+
|                             Value #N-1                        |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'wx'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                             Count                             |
+---------------+---------------+---------------+---------------+
```

#### Read Memory

Read the content of a memory device, where

* `Address` is the address in bytes of the first 32-bit word
* `Role` is the initiator role to use to access the device
* `Device` is the device to access (see [Enumerate](#enumerate-devices))
* `Count` is the number of 32-bit word to be read.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'RM'              |              12               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|               -               |         Device        | Role  |
+---------------+---------------+---------------+---------------+
|                            Address                            |
+---------------+---------------+---------------+---------------+
|                             Count                             |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'rm'              |            4..4*Count         |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                            Value #0                           |
+---------------+---------------+---------------+---------------+
|                            Value #1                           |
+---------------+---------------+---------------+---------------+
|                              ....                             |
+---------------+---------------+---------------+---------------+
|                            Value #N-1                         |
+---------------+---------------+---------------+---------------+
```

#### Write Memory

Write a buffer to a memory device

* `Address` is the address in bytes of the first 32-bit word to be written
* `Role` is the initiator role to use to access the device
* `Device` is the device to access (see [Enumerate Devices](#enumerate-devices))
* `Value` are the values to write in memory word.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'WM'              |           8 + 4*Count         |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|               -               |         Device        | Role  |
+---------------+---------------+---------------+---------------+
|                            Address                            |
+---------------+---------------+---------------+---------------+
|                            Value #0                           |
+---------------+---------------+---------------+---------------+
|                            Value #1                           |
+---------------+---------------+---------------+---------------+
|                              ....                             |
+---------------+---------------+---------------+---------------+
|                            Value #N-1                         |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'wm'              |               4               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                             Count                             |
+---------------+---------------+---------------+---------------+
```

#### Resume VM execution

Resume execution if the VM is currently stopped.

When VM is started in stod mode `-S`, proxy can be used to configure the VM before kicking off the vCPUs.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'CX'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'cx'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

#### Quit

Stop QEMU emulation and return an error code to the caller.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'QT'              |               8               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|                           Error code                          |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'qt'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

This is the last command, as QEMU should exit upon receiving this request.

#### Enumerate Device Interrupt [enumerate-irq]

Enumerate can be called by the Application to retrieve the list of interrupt groups of a supported
device. The group in the response can be further used with the [Signal Interrupt API](#signal-interrupt),
[Intercept Interrupts](#intercept-interrupt) and [Release Interrupts](#release-interrupt).

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'IE'              |               4               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|               -               |         Device        |   -   |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
+---------------+---------------+---------------+---------------+
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'ie'              |            0..36N             |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|            IRQ count          |     Group     |O|        -    |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
|            IRQ count          |     Group     |O|      -      |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
|                             ....                              |
+---------------+---------------+---------------+---------------+
|            IRQ count          |     Group     |O|      -      |
+---------------+---------------+---------------+---------------+
|                                                               |
|                                                               |
|                           Identifier                          |
|                                                               |
|                                                               |
+---------------+---------------+---------------+---------------+
```
Reponse contains 0 up to N interrupt groups, each group is described with a 36-byte entry, where:

* `IRQ count` is the count of the input interrupts for this group,
* `O` is set if IRQ in this group are output interrupts or or not set if they are input interrupts,
* `Group` is the interrupt group identifier identifier.
* `Identifier` defines the interrupt group name

The count of address spaces can be retrieved from the `LENGTH` field.

#### Intercept Interrupts [intercept-interrupt]

Route one or more device output interrupts to the proxy (vs. the internal PLIC)

* `Device` is the device to access (see [Enumerate Device](#enumerate-devices))
* `Group` is the IRQ group in the device (see [Enumerate Interrupt](#enumerate-irq))
* `Interrupt mask` define which interrupts should be released (1 bit per interrupt). The count of
  32-bit interrupt mask word can be retrieved from the header length.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'II'              |              8..              |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|     Group     |        -      |         Device        |   -   |
+---------------+---------------+---------------+---------------+
|                                                               |
|                        Interrupt mask                         |
|                                                               |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'ii'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

#### Release Interrupts [release-interrupt]

Revert any previous interception, reconnecting selected IRQ to their original
destination device.

* `Device` is the device to access (see [Enumerate Device](#enumerate-devices))
* `Group` is the IRQ group in the device (see [Enumerate Interrupt](#enumerate-irq))
* `Interrupt mask` define which interrupts should be released (1 bit per interrupt). The count of
  32-bit interrupt mask word can be retrieved from the header length.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'IR'              |              8..              |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|     Group     |        -      |         Device        |   -   |
+---------------+---------------+---------------+---------------+
|                                                               |
|                        Interrupt mask                         |
|                                                               |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'ir'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

#### Signal Interrupt [signal-interrupt]

Set or Reset an input interrupt line.

* `Device` is the device to access (see [Enumerate Device](#enumerate-devices)),
* `Group` the identifier of the IRQ group (see [Enumerate Interrupt](#enumerate-irq)),
* `Interrupt line` the index of the interrupt line to signal within the group. The interrupt line
   should range between 0 and the input IRQ count for this group,
* `Level` the new interrupt line level. Usually `1` to assert/set (1) or `0` to deassert/release,
  even if any 32-bit value is accepted.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'IS'              |               12              |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|             Group             |         Device        |   -   |
+---------------+---------------+---------------+---------------+
|          Interrupt line       |               -               |
+---------------+---------------+---------------+---------------+
|                             Level                             |
+---------------+---------------+---------------+---------------+
```

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'is'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

#### Intercept arbitrary MMIO region [intercept-region]

It is possible to get intercept access to a memory region. Any intercepted region cannot
be any longer accessed by the remote vCPU.

Use with care, as it may generate a very high traffic on the communication link. Width of
intercepted location should be kept small.

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'MI'              |              12               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|R|W| Priority  | - |   Stop    |       -       |      MSpc     |
+---------------+---------------+---------------+---------------+
|                            Address                            |
+---------------+---------------+---------------+---------------+
|                             Size                              |
+---------------+---------------+---------------+---------------+
```

* `R` whether to notify on read access
* `W` whether to notify on write access
* `Priority` is the priority order (increasing order). 0 is reserved. If not sure, use 1.
* `Stop` auto-stop count. If non-zero, only the specified count of notifications are reported,
   after which the MMIO location intercepter is automatically discarded.
* `MSpc` is the memory space of the region to select (see [Enumerate Memory Spaces](#enumerate-mr-spaces))
* `Address` is the byte address of the first byte to intercept in the selected memory space
* `Size` is the width in bytes of the region to intercept

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'mi'              |               4               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|               0               |         Region        |   -   |
+---------------+---------------+---------------+---------------+
```

* `Region` is the new intercepter number that describes the intercepting region

#### Release intercepted MMIO locations

##### Request
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'MR'              |               4               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
|               -               |         Region        |   -   |
+---------------+---------------+---------------+---------------+
```

* `Region` is the number of the intercepter to release

##### Response
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|             'mr'              |               0               |
+---------------+---------------+---------------+---------------+
|                              UID                            |0|
+---------------+---------------+---------------+---------------+
```

* `Device` is the new device number that describes the intercepting region

### Interrupt signalling

Interrupt signalling are messages that never expect a response and that may be sent/received at any
time. Each receiver should be prepared to handle many interrupt messages without any prior
request/command.

An [intercept interrupt](#intercept-interrupt) command should have been first used to select which
interrupt(s) to receive.

#### "Wired" Interrupt
```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|              '^W'             |               12              |
+---------------+---------------+---------------+---------------+
|                              UID                            |1|
+---------------+---------------+---------------+---------------+
|               -               |         Device        |   -   |
+---------------+---------------+---------------+---------------+
|            Channel            |     Group     |O|      -      |
+---------------+---------------+---------------+---------------+
|                            Value                              |
+---------------+---------------+---------------+---------------+
```

* `Device` that triggered the interrupt
* `Group` which interrupt group has been triggered
* `Channel` which interrupt channel for the group has been triggered
* `Value` the new interrupt value. For binary IRQs, `1` when IRQ is raised, `0` when it is lowered.

#### MSI Interrupt

To be defined.

```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|              '^M'             |               8               |
+---------------+---------------+---------------+---------------+
|                              UID                            |1|
+---------------+---------------+---------------+---------------+
|               -               |         Device        | Role  |
+---------------+---------------+---------------+---------------+
|                            Value                              |
+---------------+---------------+---------------+---------------+
```

#### Region Access

See [Intercept arbitrary MMIO region](#intercept-region) to register a watcher

```
+---------------+---------------+---------------+---------------+
|       0       |       1       |       2       |       3       |
|0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
+---------------+---------------+---------------+---------------+
|              '^R'             |              12               |
+---------------+---------------+---------------+---------------+
|                              UID                            |1|
+---------------+---------------+---------------+---------------+
|R|W|   | Width |       -       |         Region        |  Role |
+---------------+---------------+---------------+---------------+
|                            Address                            |
+---------------+---------------+---------------+---------------+
|                             Value                             |
+---------------+---------------+---------------+---------------+
```

* `Region` the region watcher identifier, as returned in the interception registration response
* `R` read access
* `W` write access
* `Width` the width in byte of the MMIO access (1/2/4)
* `Address` the absolute address of the MMIO access
* `Value` the value if memory location was written, 0 otherwise
