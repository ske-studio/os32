
programs/args.elf:     file format elf32-i386


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
  400015:	57                   	push   %edi
  400016:	56                   	push   %esi
  400017:	53                   	push   %ebx
  400018:	51                   	push   %ecx
  400019:	83 ec 20             	sub    $0x20,%esp
  40001c:	8b 19                	mov    (%ecx),%ebx
  40001e:	8b 79 04             	mov    0x4(%ecx),%edi
  400021:	8b 71 08             	mov    0x8(%ecx),%esi
  400024:	6a 0e                	push   $0xe
  400026:	68 b7 00 40 00       	push   $0x4000b7
  40002b:	ff 56 34             	call   *0x34(%esi)
  40002e:	83 c4 0c             	add    $0xc,%esp
  400031:	89 5d e4             	mov    %ebx,-0x1c(%ebp)
  400034:	53                   	push   %ebx
  400035:	68 d2 00 40 00       	push   $0x4000d2
  40003a:	6a 07                	push   $0x7
  40003c:	ff 56 50             	call   *0x50(%esi)
  40003f:	83 c4 10             	add    $0x10,%esp
  400042:	8b 5d e4             	mov    -0x1c(%ebp),%ebx
  400045:	85 db                	test   %ebx,%ebx
  400047:	7e 2f                	jle    400078 <main+0x70>
  400049:	31 db                	xor    %ebx,%ebx
  40004b:	eb 17                	jmp    400064 <main+0x5c>
  40004d:	8d 76 00             	lea    0x0(%esi),%esi
  400050:	52                   	push   %edx
  400051:	53                   	push   %ebx
  400052:	68 dd 00 40 00       	push   $0x4000dd
  400057:	6a 0a                	push   $0xa
  400059:	ff d1                	call   *%ecx
  40005b:	43                   	inc    %ebx
  40005c:	83 c4 10             	add    $0x10,%esp
  40005f:	39 5d e4             	cmp    %ebx,-0x1c(%ebp)
  400062:	74 14                	je     400078 <main+0x70>
  400064:	8b 4e 50             	mov    0x50(%esi),%ecx
  400067:	8b 14 9f             	mov    (%edi,%ebx,4),%edx
  40006a:	85 d2                	test   %edx,%edx
  40006c:	75 e2                	jne    400050 <main+0x48>
  40006e:	ba b0 00 40 00       	mov    $0x4000b0,%edx
  400073:	eb db                	jmp    400050 <main+0x48>
  400075:	8d 76 00             	lea    0x0(%esi),%esi
  400078:	8b 46 50             	mov    0x50(%esi),%eax
  40007b:	8b 75 e4             	mov    -0x1c(%ebp),%esi
  40007e:	8b 0c b7             	mov    (%edi,%esi,4),%ecx
  400081:	85 c9                	test   %ecx,%ecx
  400083:	74 1b                	je     4000a0 <main+0x98>
  400085:	83 ec 08             	sub    $0x8,%esp
  400088:	68 04 01 40 00       	push   $0x400104
  40008d:	6a 4f                	push   $0x4f
  40008f:	ff d0                	call   *%eax
  400091:	83 c4 10             	add    $0x10,%esp
  400094:	8d 65 f0             	lea    -0x10(%ebp),%esp
  400097:	59                   	pop    %ecx
  400098:	5b                   	pop    %ebx
  400099:	5e                   	pop    %esi
  40009a:	5f                   	pop    %edi
  40009b:	5d                   	pop    %ebp
  40009c:	8d 61 fc             	lea    -0x4(%ecx),%esp
  40009f:	c3                   	ret
  4000a0:	52                   	push   %edx
  4000a1:	52                   	push   %edx
  4000a2:	68 ee 00 40 00       	push   $0x4000ee
  4000a7:	6a 02                	push   $0x2
  4000a9:	ff d0                	call   *%eax
  4000ab:	83 c4 10             	add    $0x10,%esp
  4000ae:	eb e4                	jmp    400094 <main+0x8c>
