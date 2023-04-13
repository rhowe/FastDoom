;
; DESCRIPTION: Assembly texture mapping routines for VESA modes
;

BITS 32
%include "macros.inc"

%ifdef MODE_VBE2_DIRECT
%include "defs.inc"

extern _destview
extern _centery

;============================================================================
; unwound vertical scaling code
;
; eax   light table pointer, 0 lowbyte overwritten
; ebx   all 0, low byte overwritten
; ecx   fractional step value
; edx   fractional scale value
; esi   start of source pixels
; edi   bottom pixel in screenbuffer to blit into
;
; ebx should be set to 0 0 0 dh to feed the pipeline
;
; The graphics wrap vertically at 128 pixels
;============================================================================

BEGIN_DATA_SECTION

%macro SCALEDEFINE 1
  dd vscale%1
%endmacro

align 4

scalecalls:
  %assign LINE 0
  %rep SCREENHEIGHT+1
    SCALEDEFINE LINE
  %assign LINE LINE+1
  %endrep

;============================================================================

BEGIN_CODE_SECTION

;============================================================================
;
; R_DrawColumnLowVBE2
;
;============================================================================

CODE_SYM_DEF R_DrawColumnLowVBE2
	push		ebx
	push		ecx
	push		edx
	push		esi
	push		edi
	push		ebp

  mov  ebp,[_dc_yh]
  mov  ebx,[_dc_x]
  mov  eax,[_dc_yl]
  shl  ebx,1
  lea  edi,[ebp+ebp*4]
  sub  ebp,eax ; ebp = pixel count
  js   short .done

  shl  edi,6
  add  edi,ebx
  add  edi,[_destview]

  mov  ecx,[_dc_iscale]

  sub   eax,[_centery]
  imul  ecx
  mov   edx,[_dc_texturemid]
  shl   ecx,9 ; 7 significant bits, 25 frac
  add   edx,eax
  mov   esi,[_dc_source]
  shl   edx,9 ; 7 significant bits, 25 frac
  mov   eax,[_dc_colormap]

  xor   ebx,ebx
  shld  ebx,edx,7 ; get address of first location
  call  [scalecalls+4+ebp*4]

.done:
	pop		ebp
	pop		edi
	pop		esi
	pop		edx
	pop		ecx
	pop		ebx
  ret
; R_DrawColumnLowVBE2 ends

;============ HIGH DETAIL ============

%macro SCALELABEL 1
  vscale%1
%endmacro

%assign LINE SCREENHEIGHT
%rep SCREENHEIGHT-1
  SCALELABEL LINE:
    mov  al,[esi+ebx]                    ; get source pixel
    add  edx,ecx                         ; calculate next location
    mov  al,[eax]                        ; translate the color
    mov  ebx,edx
    mov  [edi-(LINE-1)*SCREENWIDTH],al   ; draw a pixel to the buffer
    mov  [edi-(LINE-1)*SCREENWIDTH+1],al   ; draw a pixel to the buffer
    shr  ebx,25
    %assign LINE LINE-1
%endrep

vscale1:
  mov  al,[esi+ebx]
  mov  al,[eax]
  mov  ah, al
  mov  [edi],ax

vscale0:
  ret

CONTINUE_DATA_SECTION

%macro MAPDEFINE 1
  dd hmap%1
%endmacro

align 4

mapcalls:
  %assign LINE 0
  %rep SCREENWIDTH/2+1
    MAPDEFINE LINE
    %assign LINE LINE+1
  %endrep

callpoint:   dd 0
returnpoint: dd 0

CONTINUE_CODE_SECTION

;============================================================================
;
; R_DrawSpanLowVBE2
;
; Horizontal texture mapping
;
;============================================================================
CODE_SYM_DEF R_DrawSpanLowVBE2
  push		ebx
	push		ecx
	push		edx
	push		esi
	push		edi
	push		ebp

  mov  eax,[_ds_x1]
  mov  ebx,[_ds_x2]
  mov  eax,[mapcalls+eax*4]
  mov  ecx,[_ds_frac]        ; build composite position
  mov  [callpoint],eax ; spot to jump into unwound
  mov	 ebp,[_ds_step]
  mov  eax,[mapcalls+4+ebx*4]
  mov	 esi,[_ds_source]
  mov  [returnpoint],eax ; spot to patch a ret at
  mov  [eax], byte OP_RET

  mov  edx,[_ds_y]
  mov  eax,[_ds_colormap]
  lea  edi,[edx+edx*4]
  shld  ebx,ecx,22      ; shift y units in
  shl  edi,6
  add  edi,[_destview]
  shld  ebx,ecx,6       ; shift x units in
  xor   edx,edx
  and   ebx,0x0FFF      ; mask off slop bits
  add   ecx,ebp
  
  ; feed the pipeline and jump in
  call  [callpoint]

  mov  ebx,[returnpoint]
  mov  [ebx],byte OP_MOVAL ; remove the ret patched in

	pop		ebp
	pop		edi
	pop		esi
	pop		edx
	pop		ecx
	pop		ebx
  ret
; R_DrawSpanLowVBE2 ends

;============= HIGH DETAIL ============

%macro MAPLABEL 1
  hmap%1
%endmacro

%assign LINE 0
%assign PCOL 0
%rep SCREENWIDTH/2
  %assign PLANE 0
    MAPLABEL LINE:
      %assign LINE LINE+1
      %if LINE = 160
        mov   al,[esi+ebx]           ; get source pixel
        mov   al,[eax]               ; translate color
        mov   ah,al
        mov   [edi+PLANE+PCOL*2],ax  ; write pixel
      %else
        mov   al,[esi+ebx]           ; get source pixel
        shld  ebx,ecx,22             ; shift y units in
        mov   dl,[eax]               ; translate color
        shld  ebx,ecx,6              ; shift x units in
        mov   dh,dl
        mov   [edi+PLANE+PCOL*2],dx  ; write pixel
        and   ebx,0x0FFF             ; mask off slop bits
        add   ecx,ebp                ; position += step
      %endif
      %assign PLANE PLANE+1
%assign PCOL PCOL+1
%endrep

hmap160:
  ret

%endif
