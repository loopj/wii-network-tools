#include <stdint.h>

// Number of text sections in a DOL
#define DOL_TEXT_SECTIONS 7

// Number of data sections in a DOL
#define DOL_DATA_SECTIONS 11

// Number of sections in a DOL, text first
#define DOL_SECTIONS (DOL_TEXT_SECTIONS + DOL_DATA_SECTIONS)

// Partition offset of the disc header field that holds the DOL offset, in the 4 byte units loaders read in
#define BOOT_DOL_OFFSET (0x420 >> 2)

// Data cache line size of the Broadway
#define CACHE_LINE 32

// Loading stages, one read request each until the sections
enum stage {
  // Read the disc header field holding the DOL offset
  STAGE_BOOT_FIELDS,

  // Read the DOL header
  STAGE_DOL_HEADER,

  // Read each section in turn
  STAGE_SECTIONS,
};

// Header at the start of a DOL file
struct dol_header {
  // File offset of each section, text first
  uint32_t offset[DOL_SECTIONS];

  // Load address of each section
  uint32_t address[DOL_SECTIONS];

  // Size of each section, 0 when unused
  uint32_t size[DOL_SECTIONS];

  // Start of the zero initialized area
  uint32_t bss_address;

  // Size of the zero initialized area
  uint32_t bss_size;

  // Entry point
  uint32_t entry;

  // Pads the header to 0x100 bytes
  uint32_t reserved[7];
};

// Disc header fields from 0x420, the first holds the DOL offset
static uint32_t boot_fields[8] __attribute__((aligned(CACHE_LINE)));

// Header of the DOL being loaded
static struct dol_header dol __attribute__((aligned(CACHE_LINE)));

// The stage the next read request belongs to
static enum stage stage;

// The next section to read
static int next_section;

// Drop the cached lines over a range, so a DMA write into it is read fresh
static void invalidate_data_cache(uint32_t start, uint32_t len)
{
  for (uint32_t line = start & ~(CACHE_LINE - 1u); line < start + len; line += CACHE_LINE)
    __asm__ volatile("dcbi 0,%0" ::"r"(line) : "memory");
}

// Write the cached lines over a range back to memory
static void store_data_cache(uint32_t start, uint32_t len)
{
  for (uint32_t line = start & ~(CACHE_LINE - 1u); line < start + len; line += CACHE_LINE)
    __asm__ volatile("dcbst 0,%0" ::"r"(line) : "memory");
  __asm__ volatile("sync" ::: "memory");
}

// Drop the instruction cache over a range, so the code loaded there is fetched fresh
static void invalidate_instruction_cache(uint32_t start, uint32_t len)
{
  for (uint32_t line = start & ~(CACHE_LINE - 1u); line < start + len; line += CACHE_LINE)
    __asm__ volatile("icbi 0,%0" ::"r"(line) : "memory");
  __asm__ volatile("sync; isync" ::: "memory");
}

// Start a load from the first stage
static void apploader_init(void (*report)(const char *format, ...))
{
  (void)report;
  stage        = STAGE_BOOT_FIELDS;
  next_section = 0;
}

// Hand the loader the next read, returning 0 once every section has been read
static int apploader_main(void **destination, int *size, int *offset)
{
  switch (stage) {
    case STAGE_BOOT_FIELDS:
      // Read the field that holds the DOL offset
      *destination = boot_fields;
      *size        = sizeof boot_fields;
      *offset      = BOOT_DOL_OFFSET;
      stage        = STAGE_DOL_HEADER;
      return 1;

    case STAGE_DOL_HEADER:
      // Read the DOL header, dropping any cached copy of the fields the loader wrote by DMA first
      invalidate_data_cache((uint32_t)boot_fields, sizeof boot_fields);
      *destination = &dol;
      *size        = sizeof dol;
      *offset      = boot_fields[0];
      stage        = STAGE_SECTIONS;
      return 1;

    case STAGE_SECTIONS:
      // Drop any cached copy of the header before reading it
      if (next_section == 0)
        invalidate_data_cache((uint32_t)&dol, sizeof dol);

      // Read each section with contents, rounded up to whole cache lines, after dropping its cached lines
      while (next_section < DOL_SECTIONS) {
        int i = next_section++;
        if (dol.size[i] == 0)
          continue;
        uint32_t len = (dol.size[i] + CACHE_LINE - 1) & ~(CACHE_LINE - 1u);
        invalidate_data_cache(dol.address[i], len);
        *destination = (void *)dol.address[i];
        *size        = (int)len;
        *offset      = (int)(boot_fields[0] + (dol.offset[i] >> 2));
        return 1;
      }
      return 0;
  }
  return 0;
}

// Finish the load and return the DOL's entry point
static void *apploader_final(void)
{
  // Zero the DOL's bss
  uint32_t *bss = (uint32_t *)dol.bss_address;
  for (uint32_t i = 0; i < dol.bss_size / 4; i++)
    bss[i] = 0;

  // Write the loaded sections back before fetching code, since loaders patch them with ordinary stores
  for (int i = 0; i < DOL_SECTIONS; i++) {
    if (dol.size[i] == 0)
      continue;
    store_data_cache(dol.address[i], dol.size[i]);
    if (i < DOL_TEXT_SECTIONS)
      invalidate_instruction_cache(dol.address[i], dol.size[i]);
  }

  return (void *)dol.entry;
}

// Hand the loader the three apploader functions, linked first so it sits at the load address
__attribute__((section(".text.entry"))) void apploader_entry(void (**init)(void (*)(const char *, ...)),
                                                             int (**main)(void **, int *, int *), void *(**final)(void))
{
  *init  = apploader_init;
  *main  = apploader_main;
  *final = apploader_final;
}
