section .rodata
align 16

global hello_bin
global hello_bin_end
hello_bin:
incbin "build/hello.bin"
hello_bin_end:

global ticker_bin
global ticker_bin_end
ticker_bin:
incbin "build/ticker.bin"
ticker_bin_end:

global pulse_bin
global pulse_bin_end
pulse_bin:
incbin "build/pulse.bin"
pulse_bin_end:

global crashy_bin
global crashy_bin_end
crashy_bin:
incbin "build/crashy.bin"
crashy_bin_end:

global open_bin
global open_bin_end
open_bin:
incbin "build/open.bin"
open_bin_end:

global vtxt_bin
global vtxt_bin_end
vtxt_bin:
incbin "build/vtxt.bin"
vtxt_bin_end:

global vdocx_bin
global vdocx_bin_end
vdocx_bin:
incbin "build/vdocx.bin"
vdocx_bin_end:

global vimg_bin
global vimg_bin_end
vimg_bin:
incbin "build/vimg.bin"
vimg_bin_end:

global vpdf_bin
global vpdf_bin_end
vpdf_bin:
incbin "build/vpdf.bin"
vpdf_bin_end:

global vmp4_bin
global vmp4_bin_end
vmp4_bin:
incbin "build/vmp4.bin"
vmp4_bin_end:

global sample_note_txt
global sample_note_txt_end
sample_note_txt:
incbin "samples/note.txt"
sample_note_txt_end:

global sample_note_docx
global sample_note_docx_end
sample_note_docx:
incbin "samples/note.docx"
sample_note_docx_end:

global sample_pic_png
global sample_pic_png_end
sample_pic_png:
incbin "samples/pic.png"
sample_pic_png_end:

global sample_pic_jpg
global sample_pic_jpg_end
sample_pic_jpg:
incbin "samples/pic.jpg"
sample_pic_jpg_end:

global sample_note_pdf
global sample_note_pdf_end
sample_note_pdf:
incbin "samples/note.pdf"
sample_note_pdf_end:

global sample_clip_mp4
global sample_clip_mp4_end
sample_clip_mp4:
incbin "samples/clip.mp4"
sample_clip_mp4_end:

global pkg_repo
global pkg_repo_end
pkg_repo:
incbin "build/pkgrepo.bin"
pkg_repo_end:
