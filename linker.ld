OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(kmain)

PHDRS {
    text   PT_LOAD FLAGS((1 << 0) | (1 << 2));  /* r-x */
    rodata PT_LOAD FLAGS((1 << 2));              /* r-- */
    data   PT_LOAD FLAGS((1 << 1) | (1 << 2));  /* rw- */
}

SECTIONS {
    . = 0xffffffff80000000;

    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .limine_requests_start : {
        KEEP(*(.limine_requests_start))
    } :rodata

    .limine_requests : {
        KEEP(*(.limine_requests))
    } :rodata

    .limine_requests_end : {
        KEEP(*(.limine_requests_end))
    } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    __data_start = .;
    .data : {
        *(.data .data.*)
    } :data
    __data_end = .;

    __bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data
    __bss_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
