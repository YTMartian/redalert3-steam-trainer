MC:
jmp MC+0x1200
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x29:
Pushfd
Pushad
mov ecx,[IDB+4]
cmp [eax+0x0c],ecx
.byte 0x75,0x17
cmp byte ptr [FLAGS+8],0x01
.byte 0x75,0x0e
mov byte ptr [FLAGS+8],0x00
add dword ptr [eax+0x04], 0x186A0
Popad
Popfd
add edi,[eax+4]
mov edx,[ecx]
mov eax,[FLAGS+0x20]
test eax,eax
jne MC+0x600
jmp _BackPlayerMoney
nop
nop
nop
nop
nop
nop
MC+0x6c:
Pushfd
Pushad
cmp byte ptr [FLAGS+9],0x01
.byte 0x75,0x0e
mov dword ptr [eax+0x04], 0x000186A0
mov dword ptr [eax+0x08], 0x00000000
Popad
Popfd
mov eax,[eax+0x04]
mov ecx,[esi+0x000003B0]
jmp _BackPlayerPower
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x9F:
pushfd
pushad
cmp byte ptr [FLAGS+0xA],0x01
.byte 0x75,0x07
mov dword ptr [eax+0x34], 0x0000000f
popad
popfd
mov edi,[eax+0x34]
mov ecx,[esi+0x3c]
jmp _BackPlayerSCPoint
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0xc8:
pushfd
pushad
cmp byte ptr [FLAGS+0xB],0x01
.byte 0x75,0x07
mov dword ptr [edi+0x2c], 0x43c78000
popad
popfd
movss xmm0,[edi+0x2c]
jmp _BackPlayerHaveAllSC
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0xf0:
pushfd
pushad
cmp byte ptr [FLAGS+0xC],0x01
.byte 0x75,0x07
mov dword ptr [esi+0x1c], 0x42c80000
popad
popfd
cvttss2si eax,[esi+0x1c]
jmp _BackPlayerFastBuild
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x118:
pushfd
pushad
cmp byte ptr [FLAGS+0xC],0x01
.byte 0x75,0x07
mov dword ptr [esi+0x1c], 0x42c80000
popad
popfd
fld dword ptr [esi+0x1c]
mov [esi+0x5c],eax
jmp _BackPlayerFastBuild2
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x141:
pushfd
pushad
cmp byte ptr [FLAGS+0xC],0x01
.byte 0x75,0x07
cmp ebp,0x01
.byte 0x74,0x02
.byte 0xeb,0x0d
popad
popfd
fld dword ptr [esi+0x000001bc]
jmp _BackPlayerFastBuild3
pushad
mov eax,[edi+0x3c]
inc eax
mov [edi+0x14],eax
popad
.byte 0xeb,0xe8
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x175:
pushfd
pushad
cmp byte ptr [FLAGS+0xD],0x01
.byte 0x75,0x15
mov ecx,[IDB]
cmp [eax+0x00000418],ecx
.byte 0x75,0x07
mov dword ptr [esi+0x20], 0x00000001
cmp byte ptr [FLAGS+0xE],0x01
.byte 0x75,0x15
mov ecx,[IDB]
cmp [eax+0x00000418],ecx
.byte 0x74,0x07
.byte 0x81,0x46,0x20,0x01,0x00,0x00,0x00
popad
popfd
mov ebx,[eax+0x00000418]
jmp _BackPlayerSuperPower
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x1ca:
mov esi,[eax+0x50]
mov eax,[ecx]
pushfd
pushad
cmp byte ptr [FLAGS+0xD],0x01
.byte 0x75,0x13
mov ecx,[IDB]
cmp [esp+0x28],ecx
.byte 0x75,0x07
mov dword ptr [eax+0x0c], 0x00000000
popad
popfd
jmp _BackPlayerSuperPower2
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x250:
mov edx,[ecx+0x50]
cmp edx,[eax+0x0c]
pushfd
pushad
cmp byte ptr [FLAGS+0xE],0x01
.byte 0x75,0x07
.byte 0x81,0x40,0x0c,0x01,0x00,0x00,0x00
popad
popfd
jmp _BackDisableAllSuperPower
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x294:
mov ecx,[eax+0x50]
cmp ecx,[esi+0x20]
jmp _BackDisableAllSuperPower2
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x2bd:
movss [esi+0x44],xmm0
cmp byte ptr [FLAGS+0xF],1
.byte 0x75,0x0b
jmp _BackPlayerZoom
fadd dword ptr [esi+0x44]
fstp dword ptr [esi+0x44]
cmp byte ptr [FLAGS+0x10],1
.byte 0x75,0x0b
jmp _BackPlayerZoom
fsub dword ptr [esi+0x44]
fstp dword ptr [esi+0x44]
jmp _BackPlayerZoom
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x2f9:
movss [ebp+0x00000260],xmm0
pushfd
pushad
cmp byte ptr [FLAGS+0x11],0x01
.byte 0x75,0x0a
mov dword ptr [ebp+0x00000260], 0x47c35000
popad
popfd
jmp _BackPlayerMap
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x327:
mov ecx,[edx+edi*4]
test ecx,ecx
pushfd
pushad
cmp byte ptr [FLAGS+0x12],0x01
.byte 0x75,0x13
.byte 0x81,0xbd,0xac,0x03,0x00,0x00,0x00,0x00,0x00,0x00
.byte 0x74,0x07
mov dword ptr [ecx+0x1c], 0x7FFFFFFF
popad
popfd
jmp _BackSelectUnitAmmo
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x35b:
mov eax,[ecx+0x00001278]
pushfd
pushad
cmp byte ptr [FLAGS+0x13],0x01
.byte 0x75,0x07
mov dword ptr [eax+0x08], 0x0000c350
cmp byte ptr [FLAGS+0x13],0x02
.byte 0x75,0x07
mov dword ptr [eax+0x08], 0x00000000
popad
popfd
jmp _BackDangerLevel
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x394:
mov eax,[eax+0x08]
pushfd
pushad
cmp ebx,0x00
.byte 0x75,0x17
cmp byte ptr [FLAGS+0x14],0x01
.byte 0x75,0x0e
mov byte ptr [FLAGS+0x14],0x00
mov dword ptr [ecx+0x14], 0x00000000
popad
popfd
sub eax,[ecx+0x14]
jmp _BackRestoreOreMine
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x3d0:
pushfd
pushad
cmp byte ptr [FLAGS+0x15],0x00
.byte 0xEB,0x12
mov ecx,[IDB+4]
cmp [eax+0x0c],ecx
.byte 0x74,0x07
mov dword ptr [eax+0x04], 0x00001388
cmp byte ptr [FLAGS+0x15],0x01
.byte 0x75,0x12
mov ecx,[IDB+4]
cmp [eax+0x0c],ecx
.byte 0x74,0x07
mov dword ptr [eax+0x04], 0x00000001
popad
popfd
add edx,[eax+0x04]
cmp edx,edi
jmp _BackEnemyCantBuild
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x41e:
pushfd
pushad
cmp byte ptr [FLAGS+0x16],1
.byte 0x75,0x22
mov ecx,[ebx+0x138]
mov edi,[ecx+0x418]
cmp edi,[IDB]
.byte 0x75,0x0e
mov dword ptr [esi+4], 0x497423f0
mov dword ptr [esi+0xc], 0x497423f0
popad
popfd
mov edx,[eax+0x3c]
mov ecx,esi
jmp _BackPlayerGodMode
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x461:
push eax
cmp esi,[IDB+0x10]
jne _ExitPlayerOneKillItMode
mov eax,[esi-0x08]
or eax,eax
jz _ExitPlayerOneKillItMode
push ebx
mov ebx,[eax+0x00000390]
or ebx,[eax+0x00000394]
or ebx,[eax+0x000003a0]
or ebx,[eax+0x000003ac]
pop ebx
jz _ExitPlayerOneKillItMode
cmp byte ptr [FLAGS+0x17],1
jne _ExitPlayerOneKillItMode
mov eax,[eax+0x00000418]
mov eax,[eax+0x20]
cmp eax,[IDB+4]
je _ExitPlayerOneKillItMode
mov [IDB+8],esi
sub eax,eax
mov [esi+0x04],eax
mov [esi+0x0c],eax
movss xmm0,[esi+0x0c]
_ExitPlayerOneKillItMode:
movss [esi+0x04],xmm0
pop eax
test eax,eax
jmp _BackPlayerOneKillItMode
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x4e5:
mov ecx,[esi+0x0000033c]
mov [IDB+0xC],esi
mov [IDB+0x10],ecx
jmp _BackPlayerOneKillItModeData
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x506:
mov [IDB+0xC],ecx
mov ecx,[ecx+0x0000033c]
mov [IDB+0x10],ecx
jmp _BackPlayerOneKillItModeData2
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x600:
pushad
call MC+0x700
popad
xor eax,eax
mov [FLAGS+0x20],eax
jmp _BackPlayerMoney
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x700:
mov eax,[FLAGS+0x20]
cmp eax,1
je MC2
cmp eax,2
je MC2+0x100
cmp eax,3
je MC2+0x200
cmp eax,4
je MC2+0x300
cmp eax,5
je MC2+0x400
cmp eax,6
je MC2+0x500
cmp eax,7
je MC2+0x600
cmp eax,8
je MC2+0x700
cmp eax,9
je MC2+0x800
cmp eax,0xA
je MC2+0x900
cmp eax,0xB
je MC2+0xA00
cmp eax,0xC
je MC2+0xB00
cmp eax,0xD
je MC2+0xc00
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x800:
mov ecx,eax
push 0
push 1
push 3
call MOD+0x35C200
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x900:
mov ecx,eax
push 0
push 0x19
push 6
call MOD+0x39EA50
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0xA00:
mov eax,[MOD+0x8E08DC]
mov eax,[eax+0x5c]
test eax,eax
.byte 0x75,0x04
add esp,4
ret
mov eax,[MOD+0x8E08DC]
mov eax,[eax+0x50]
mov eax,[eax+8]
mov esi,[eax+0x138]
test esi,esi
.byte 0x75,0x04
add esp,4
ret
mov [MC+0x1000],esi
mov eax,[esi+0x38]
mov [MC+0x1080],eax
mov eax,[esi+0x3c]
mov [MC+0x1084],eax
mov eax,[esi+0x40]
mov [MC+0x1088],eax
mov eax,[esi+4]
mov [MC+0x1020],eax
mov eax,[esi+0x418]
mov [MC+0x1040],eax
ret
nop
nop
nop
nop
nop
MC+0xA60:
push eax
mov ecx,[MOD+0x8E6C58]
call MOD+0x3E4230
mov [MC+0x1020],eax
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0xAA0:
mov ecx,[MC+0x1020]
test ecx,ecx
.byte 0x75,0x01
ret
mov eax,[MC+0x1040]
mov ebx,[eax+0x10]
push ebx
push eax
lea eax,[MC+0x1080]
push eax
push ecx
push 0
call MOD+0x205240
add esp,0x14
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0xB00:
or dword ptr [eax+0xbc], 0x80000000
or dword ptr [eax+0xc8], 0x00000800
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x1120:
mov eax,[MOD+0x8daefc]
mov ebx,[eax+0x3c]
mov [MC+0x1110],ebx
mov ebx,[eax+0x40]
mov [MC+0x1114],ebx
mov ebx,[eax+0x44]
mov [MC+0x111c],ebx
push 0
push 0
lea eax,[MC+0x1080]
push eax
lea eax,[MC+0x1110]
push eax
call MOD+0x1ed4a0
add esp,0x10
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC+0x1200:
pushad
mov edx,[eax+0x28]
test edx,edx
jz _ExitPlayerIDClear
mov [IDB],edx
mov eax,[edx+0x20]
mov [IDB+4],eax
jmp _ExitPlayerIDAfterStore
_ExitPlayerIDClear:
xor eax,eax
mov [IDB],eax
mov [IDB+4],eax
_ExitPlayerIDAfterStore:
popad
mov edx,[eax+0x28]
test edx,edx
jz _ExitPlayerIDNoLocal
mov eax,[edx+0x20]
jmp _BackPlayerID
_ExitPlayerIDNoLocal:
xor al,al
add esp,0x0c
ret 4
MC2:
mov esi,[MOD+0x8E08DC]
mov ecx,[esi+0x5c]
test ecx,ecx
.byte 0x75,0x01
ret
mov edi,[esi+0x50]
MC2+0x11:
mov ebx,[edi+8]
test ebx,ebx
jz MC2+0x9B
mov eax,[ebx+0x138]
test eax,eax
.byte 0x75,0x01
ret
mov eax,[eax+0x374]
test eax,eax
jz MC2+0x9B
mov eax,[eax+0x200]
test eax,eax
jz MC2+0x9B
mov ebx,[eax]
test ebx,ebx
jz MC2+0x9B
cmp dword ptr [ebx+8], 0x43fa0000
.byte 0x74,0x23
cmp dword ptr [ebx+8], 0x41200000
.byte 0x74,0x13
.byte 0x81,0x7b,0x08,0x00,0x00,0x00,0x00
.byte 0x74,0x0a
movss xmm0,[ebx+8]
movss [ebx+0x40],xmm0
mov dword ptr [ebx+8], 0x43fa0000
test ebx,ebx
.byte 0x75,0x01
ret
mov eax,[eax+4]
test eax,eax
jz MC2+0x9B
cmp dword ptr [eax+0x18], 0x3f800000
.byte 0x75,0x2c
cmp dword ptr [eax+8], 0x43fa0000
.byte 0x74,0x23
cmp dword ptr [eax+8], 0x41200000
.byte 0x74,0x13
.byte 0x81,0x78,0x08,0x00,0x00,0x00,0x00
.byte 0x74,0x0a
movss xmm1,[eax+8]
movss [eax+0x40],xmm1
mov dword ptr [eax+8], 0x43fa0000
MC2+0x9B:
dec ecx
.byte 0x75,0x01
ret
mov edi,[edi]
jmp MC2+0x11
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x100:
mov esi,[MOD+0x8E08DC]
mov ecx,[esi+0x5c]
test ecx,ecx
.byte 0x75,0x01
ret
mov edi,[esi+0x50]
MC2+0x111:
mov ebx,[edi+8]
test ebx,ebx
jz MC2+0x19b
mov eax,[ebx+0x138]
test eax,eax
.byte 0x75,0x01
ret
mov eax,[eax+0x374]
test eax,eax
jz MC2+0x19b
mov eax,[eax+0x200]
test eax,eax
jz MC2+0x19b
mov ebx,[eax]
test ebx,ebx
jz MC2+0x19b
cmp dword ptr [ebx+8], 0x43fa0000
.byte 0x74,0x1c
cmp dword ptr [ebx+8], 0x41200000
.byte 0x74,0x1a
.byte 0x81,0x7b,0x08,0x00,0x00,0x00,0x00
.byte 0x74,0x0a
movss xmm0,[ebx+8]
movss [ebx+0x40],xmm0
mov dword ptr [ebx+8], 0x41200000
test ebx,ebx
.byte 0x75,0x01
ret
mov eax,[eax+4]
test eax,eax
jz MC2+0x19b
cmp dword ptr [eax+0x18], 0x3f800000
.byte 0x75,0x2c
cmp dword ptr [eax+8], 0x43fa0000
.byte 0x74,0x1c
cmp dword ptr [eax+8], 0x41200000
.byte 0x74,0x1a
.byte 0x81,0x78,0x08,0x00,0x00,0x00,0x00
.byte 0x74,0x0a
movss xmm1,[eax+8]
movss [eax+0x40],xmm1
mov dword ptr [eax+8], 0x41200000
MC2+0x19b:
dec ecx
.byte 0x75,0x01
ret
mov edi,[edi]
jmp MC2+0x111
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x200:
mov esi,[MOD+0x8E08DC]
mov ecx,[esi+0x5c]
test ecx,ecx
.byte 0x75,0x01
ret
mov edi,[esi+0x50]
MC2+0x211:
mov ebx,[edi+8]
test ebx,ebx
jz MC2+0x29b
mov eax,[ebx+0x138]
test eax,eax
.byte 0x75,0x01
ret
mov eax,[eax+0x374]
test eax,eax
jz MC2+0x29b
mov eax,[eax+0x200]
test eax,eax
jz MC2+0x29b
mov ebx,[eax]
test ebx,ebx
jz MC2+0x29b
cmp dword ptr [ebx+8], 0x43fa0000
.byte 0x74,0x1c
cmp dword ptr [ebx+8], 0x41200000
.byte 0x74,0x13
.byte 0x81,0x7b,0x08,0x00,0x00,0x00,0x00
.byte 0x74,0x11
movss xmm0,[ebx+8]
movss [ebx+0x40],xmm0
mov dword ptr [ebx+8], 0x00000000
test ebx,ebx
.byte 0x75,0x01
ret
mov eax,[eax+4]
test eax,eax
jz MC2+0x29b
cmp dword ptr [eax+0x18], 0x3f800000
.byte 0x75,0x2c
cmp dword ptr [eax+8], 0x43fa0000
.byte 0x74,0x1c
cmp dword ptr [eax+8], 0x41200000
.byte 0x74,0x13
.byte 0x81,0x78,0x08,0x00,0x00,0x00,0x00
.byte 0x74,0x11
movss xmm1,[eax+8]
movss [eax+0x40],xmm1
mov dword ptr [eax+8], 0x00000000
MC2+0x29b:
dec ecx
.byte 0x75,0x01
ret
mov edi,[edi]
jmp MC2+0x211
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x300:
mov esi,[MOD+0x8E08DC]
mov ecx,[esi+0x5c]
test ecx,ecx
.byte 0x75,0x01
ret
mov edi,[esi+0x50]
MC2+0x311:
mov ebx,[edi+8]
test ebx,ebx
jz MC2+0x391
mov eax,[ebx+0x138]
test eax,eax
.byte 0x75,0x01
ret
mov eax,[eax+0x374]
test eax,eax
jz MC2+0x391
mov eax,[eax+0x200]
test eax,eax
jz MC2+0x391
mov ebx,[eax]
test ebx,ebx
jz MC2+0x391
cmp dword ptr [ebx+8], 0x43fa0000
.byte 0x74,0x14
cmp dword ptr [ebx+8], 0x41200000
.byte 0x74,0x0b
.byte 0x81,0x7b,0x08,0x00,0x00,0x00,0x00
.byte 0x74,0x02
.byte 0xeb,0x0a
movss xmm0,[ebx+0x40]
movss [ebx+8],xmm0
test ebx,ebx
.byte 0x75,0x01
ret
mov eax,[eax+4]
test eax,eax
jz MC2+0x391
cmp dword ptr [eax+0x18], 0x3f800000
.byte 0x75,0x27
cmp dword ptr [eax+8], 0x43fa0000
.byte 0x74,0x14
cmp dword ptr [eax+8], 0x41200000
.byte 0x74,0x0b
.byte 0x81,0x78,0x08,0x00,0x00,0x00,0x00
.byte 0x74,0x02
.byte 0xeb,0x0a
movss xmm1,[eax+0x40]
movss [eax+8],xmm1
MC2+0x391:
dec ecx
.byte 0x75,0x01
ret
mov edi,[edi]
jmp MC2+0x311
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x400:
mov esi,[MOD+0x8E08DC]
mov ecx,[esi+0x5c]
test ecx,ecx
.byte 0x75,0x01
ret
mov edi,[esi+0x50]
MC2+0x411:
mov ebx,[edi+8]
test ebx,ebx
jz MC2+0x433
mov ebx,[ebx+0x138]
test ebx,ebx
jz MC2+0x433
mov eax,[ebx+0x33c]
test eax,eax
.byte 0x75,0x01
ret
mov dword ptr [eax+4], 0x497423f0
mov dword ptr [eax+0xc], 0x497423f0
MC2+0x433:
dec ecx
.byte 0x75,0x01
ret
mov edi,[edi]
jmp MC2+0x411
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x500:
mov esi,[MOD+0x8E08DC]
mov ecx,[esi+0x5c]
test ecx,ecx
.byte 0x75,0x01
ret
mov edi,[esi+0x50]
MC2+0x511:
mov ebx,[edi+8]
test ebx,ebx
jz MC2+0x52c
mov ebx,[ebx+0x138]
test ebx,ebx
jz MC2+0x52c
mov eax,[ebx+0x33c]
test eax,eax
.byte 0x75,0x01
ret
mov dword ptr [eax+4], 0x3F800000
MC2+0x52c:
dec ecx
.byte 0x75,0x01
ret
mov edi,[edi]
jmp MC2+0x511
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x600:
mov esi,[MOD+0x8E08DC]
mov ecx,[esi+0x5c]
test ecx,ecx
.byte 0x75,0x01
ret
mov edi,[esi+0x50]
MC2+0x611:
mov ebx,[edi+8]
test ebx,ebx
jz MC2+0x62e
mov ebx,[ebx+0x138]
test ebx,ebx
jz MC2+0x62e
mov eax,[ebx+0x33c]
test eax,eax
.byte 0x75,0x01
ret
mov edx,[eax+0x10]
mov [eax+4],edx
mov [eax+0xc],edx
MC2+0x62e:
dec ecx
.byte 0x75,0x01
ret
mov edi,[edi]
jmp MC2+0x611
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x700:
xor eax,eax
mov [FLAGS+0x20],eax
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x800:
mov esi,[MOD+0x8E08DC]
mov ecx,[esi+0x5c]
test ecx,ecx
.byte 0x75,0x01
ret
mov edi,[esi+0x50]
MC2+0x811:
mov ebx,[edi+8]
mov ebx,[ebx+0x138]
test ebx,ebx
.byte 0x75,0x01
ret
cmp dword ptr [MOD+0x8E9838], 0x95D6E965
.byte 0x74,0x47
cmp dword ptr [MOD+0x8E9838], 0xFD87E82A
.byte 0x74,0x3b
cmp dword ptr [MOD+0x8E9838], 0x856C9DD6
.byte 0x74,0x2f
cmp dword ptr [MOD+0x8E9838], 0xD20552E1
.byte 0x74,0x23
cmp dword ptr [MOD+0x8E9838], 0x1AFC9A6E
.byte 0x74,0x17
cmp dword ptr [MOD+0x8E9838], 0xD6D22475
.byte 0x74,0x0b
mov eax,[IDB]
mov [ebx+0x418],eax
MC2+0x872:
dec ecx
.byte 0x75,0x01
ret
mov edi,[edi]
jmp MC2+0x811
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0x900:
mov esi,[MOD+0x8E08DC]
mov eax,[esi+0x5c]
test eax,eax
.byte 0x75,0x01
ret
mov edi,[esi+0x54]
mov eax,[edi+8]
test eax,eax
.byte 0x75,0x01
ret
mov eax,[eax+0x138]
call MC+0x900
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0xA00:
call MC+0x1120
mov eax,[IDB]
mov [MC+0x1040],eax
mov eax,0x28DA574E
call MC+0xA60
call MC+0xAA0
mov eax,0xAF4C0DA5
call MC+0xA60
call MC+0xAA0
mov eax,0x1C2EF767
call MC+0xA60
call MC+0xAA0
ret
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0xB00:
call MC+0x1120
mov eax,[IDB]
mov [MC+0x1040],eax
mov eax,[FLAGS+0x24]
call MC+0xA60
test eax,eax
.byte 0x75,0x01
ret
mov [MC+0x1020],eax
push 0x08
pop eax
mov [MC+0x1100],eax
jmp MC2+0xB33
MC2+0xB33:
mov eax,[MC+0x1100]
test eax,eax
.byte 0x75,0x01
ret
call MC+0xAA0
call MC+0xB00
mov eax,[eax+0x3cc]
call MC+0x800
dec dword ptr [MC+0x1100]
jmp MC2+0xB33
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
nop
MC2+0xc00:
call MC+0x1120
mov eax,[IDB]
mov [MC+0x1040],eax
mov eax,[MOD+0x8E9838]
call MC+0xA60
call MC+0xAA0
ret