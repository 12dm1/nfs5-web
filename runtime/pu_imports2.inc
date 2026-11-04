/* nfs5-web: smaller Win32 imports PU calls that the nfsu2-web layer lacks
 * (included by pu_imports.c; same ARG/*r conventions). */

/* named shared memory: CreateFileMappingA(INVALID_HANDLE_VALUE, sa, prot,
 * sizeHigh, sizeLow, name) / OpenFileMappingA / MapViewOfFile / Unmap.  PU
 * (0x582D20) creates 15 named blocks and checks ERROR_ALREADY_EXISTS; one
 * process here, so a block is plain guest memory shared by name. */
#define MAXMAP 64
static struct { uint32_t h, base, size; char name[64]; } g_map[MAXMAP];
static uint32_t g_nmap;
void win32_set_last_error(CPU *c, uint32_t e);
int64_t fileio_handle_size(uint32_t h);
int64_t fileio_read_at(uint32_t h, uint32_t dst, uint32_t len, int64_t off);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);

static int pu_call_misc(CPU *c, const char *d, const char *n, uint32_t *r){
    if (!strcasecmp(d, "KERNEL32.dll")){
        if (!strcmp(n, "CreateFileMappingA") || !strcmp(n, "OpenFileMappingA")){
            int open = n[0] == 'O';
            uint32_t pname = open ? ARG(2) : ARG(5), size = open ? 0 : ARG(4);
            char nm[64] = "";
            if (pname) snprintf(nm, sizeof nm, "%s", (const char *)(g_mem + pname));
            if (!open && ARG(0) != 0xFFFFFFFFu){
                /* a mapping of a file: a private copy of its contents (PU maps
                 * files read-only; writes through a view would not reach the file) */
                int64_t fsz = fileio_handle_size(ARG(0));
                uint32_t want = size ? size : (uint32_t)(fsz > 0 ? fsz : 0);
                if (fsz < 0 || !want || g_nmap >= MAXMAP){ win32_set_last_error(c, 6u); *r = 0; return 1; }
                uint32_t i = g_nmap++;
                g_map[i].h = 0x00E10000u + i; g_map[i].size = want; g_map[i].name[0] = 0;
                g_map[i].base = gheap_alloc(1, want, 1);
                fileio_read_at(ARG(0), g_map[i].base, (uint32_t)(fsz < (int64_t)want ? fsz : want), 0);
                win32_set_last_error(c, 0); *r = g_map[i].h; return 1; }
            for (uint32_t i = 0; i < g_nmap; i++)
                if (nm[0] && !strcmp(g_map[i].name, nm)){ win32_set_last_error(c, 183u); *r = g_map[i].h; return 1; }  /* ERROR_ALREADY_EXISTS */
            if (open || g_nmap >= MAXMAP || !size){ win32_set_last_error(c, 2u); *r = 0; return 1; }
            uint32_t i = g_nmap++;
            g_map[i].h = 0x00E10000u + i; g_map[i].size = size;
            g_map[i].base = gheap_alloc(1, size, 1);
            snprintf(g_map[i].name, sizeof g_map[i].name, "%s", nm);
            win32_set_last_error(c, 0); *r = g_map[i].h; return 1; }
        if (!strcmp(n, "MapViewOfFile")){
            uint32_t h = ARG(0), off = ARG(3);
            *r = 0;
            for (uint32_t i = 0; i < g_nmap; i++) if (g_map[i].h == h && off < g_map[i].size) *r = g_map[i].base + off;
            return 1; }
        if (!strcmp(n, "UnmapViewOfFile")){ *r = 1; return 1; }
        if (!strcmp(n, "SetEnvironmentVariableA")){ *r = 1; return 1; }
        if (!strcmp(n, "CompareStringA") || !strcmp(n, "CompareStringW")){
            /* (lcid, flags, s1, n1, s2, n2) -> CSTR_LESS_THAN 1 / EQUAL 2 / GREATER 3 */
            int w = n[13] == 'W', ic = ARG(1) & 1u;
            uint32_t s1 = ARG(2), s2 = ARG(4); int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
            int cmp = 0;
            for (int32_t k = 0; ; k++){
                int e1 = n1 >= 0 ? k >= n1 : 0, e2 = n2 >= 0 ? k >= n2 : 0;
                uint32_t a = e1 ? 0 : (w ? MR16(s1 + 2u * k) : MR8(s1 + k));
                uint32_t b = e2 ? 0 : (w ? MR16(s2 + 2u * k) : MR8(s2 + k));
                if (ic){ if (a >= 'a' && a <= 'z') a -= 32; if (b >= 'a' && b <= 'z') b -= 32; }
                if (a != b){ cmp = a < b ? -1 : 1; break; }
                if (!a) break;
            }
            *r = (uint32_t)(cmp + 2); return 1; }
    }
    if (!strcasecmp(d, "USER32.dll")){
        if (!strcmp(n, "GetKeyState") || !strcmp(n, "GetAsyncKeyState")){ *r = 0; return 1; }   /* nothing held (WEB1) */
        if (!strcmp(n, "SetActiveWindow")){ *r = ARG(0); return 1; }
        if (!strcmp(n, "SystemParametersInfoA")){
            uint32_t act = ARG(0), pv = ARG(2);
            if (act == 0x30u && pv){                    /* SPI_GETWORKAREA */
                unsigned w, h; void metal_game_size(unsigned*, unsigned*); metal_game_size(&w, &h);
                MR32(pv) = 0; MR32(pv + 4) = 0; MR32(pv + 8) = w; MR32(pv + 12) = h; }
            else if (pv && (act & 0xFF00u) == 0 && (act == 0x10u || act == 0x72u || act == 0x3Au)) MR32(pv) = 0;  /* screensaver/... "off" */
            *r = 1; return 1; }
    }
    if (!strcasecmp(d, "GDI32.dll") && !strcmp(n, "GetStockObject")){ *r = 0x00E20000u + ARG(0); return 1; }
    if (!strcasecmp(d, "ADVAPI32.dll") && !strcmp(n, "RegFlushKey")){ *r = 0; return 1; }   /* the registry file is written on each set */
    return 0;
}
