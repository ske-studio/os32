
programs/hello.elf:     file format elf32-i386


Disassembly of section .text:

00400000 <_start>:
  400000:	e9 03 00 00 00       	jmp    400008 <main>
  400005:	66 90                	xchg   %ax,%ax
  400007:	90                   	nop

00400008 <main>:
  400008:	8d 4c 24 04          	lea    0x4(%esp),%ecx
  40000c:	83 e4 f0             	and    $0xfffffff0,%esp
  40000f:	ff 71 fc             	push   -0x4(%ecx)
  400012:	55                   	push   %ebp
  400013:	89 e5                	mov    %esp,%ebp
  400015:	53                   	push   %ebx
  400016:	51                   	push   %ecx
  400017:	8b 59 08             	mov    0x8(%ecx),%ebx
  40001a:	83 ec 08             	sub    $0x8,%esp
  40001d:	68 a1 00 00 00       	push   $0xa1
  400022:	68 b0 00 40 00       	push   $0x4000b0
  400027:	ff 53 34             	call   *0x34(%ebx)
  40002a:	58                   	pop    %eax
  40002b:	5a                   	pop    %edx
  40002c:	68 e1 00 00 00       	push   $0xe1
  400031:	68 d6 00 40 00       	push   $0x4000d6
  400036:	ff 53 34             	call   *0x34(%ebx)
  400039:	83 c4 10             	add    $0x10,%esp
  40003c:	81 3b 49 50 41 4b    	cmpl   $0x4b415049,(%ebx)
  400042:	74 57                	je     40009b <main+0x93>
  400044:	52                   	push   %edx
  400045:	52                   	push   %edx
  400046:	6a 41                	push   $0x41
  400048:	68 f3 00 40 00       	push   $0x4000f3
  40004d:	ff 53 34             	call   *0x34(%ebx)
  400050:	83 c4 10             	add    $0x10,%esp
  400053:	83 ec 08             	sub    $0x8,%esp
  400056:	68 e1 00 00 00       	push   $0xe1
  40005b:	68 fd 00 40 00       	push   $0x4000fd
  400060:	ff 53 34             	call   *0x34(%ebx)
  400063:	83 c4 10             	add    $0x10,%esp
  400066:	83 7b 04 01          	cmpl   $0x1,0x4(%ebx)
  40006a:	75 12                	jne    40007e <main+0x76>
  40006c:	50                   	push   %eax
  40006d:	50                   	push   %eax
  40006e:	68 c1 00 00 00       	push   $0xc1
  400073:	68 0d 01 40 00       	push   $0x40010d
  400078:	ff 53 34             	call   *0x34(%ebx)
  40007b:	83 c4 10             	add    $0x10,%esp
  40007e:	83 ec 08             	sub    $0x8,%esp
  400081:	68 a1 00 00 00       	push   $0xa1
  400086:	68 10 01 40 00       	push   $0x400110
  40008b:	ff 53 34             	call   *0x34(%ebx)
  40008e:	83 c4 10             	add    $0x10,%esp
  400091:	8d 65 f8             	lea    -0x8(%ebp),%esp
  400094:	59                   	pop    %ecx
  400095:	5b                   	pop    %ebx
  400096:	5d                   	pop    %ebp
  400097:	8d 61 fc             	lea    -0x4(%ecx),%esp
  40009a:	c3                   	ret
  40009b:	51                   	push   %ecx
  40009c:	51                   	push   %ecx
  40009d:	68 c1 00 00 00       	push   $0xc1
  4000a2:	68 ea 00 40 00       	push   $0x4000ea
  4000a7:	ff 53 34             	call   *0x34(%ebx)
  4000aa:	83 c4 10             	add    $0x10,%esp
  4000ad:	eb a4                	jmp    400053 <main+0x4b>
