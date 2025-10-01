#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

const double global_tol = 1e-12;
typedef struct {
    const char *in_path;
    char out_path[1024];
    double overscan;
    double power;
    double tol;              // grouping tolerance
    double feed;             // cut feed for G1
} Options;

typedef struct {
    double x1,y1,x2,y2;
    double power;
    int has_power;
} Segment;

typedef struct {
    double key;       
    Segment *arr;
    size_t len, cap;
} Group;

typedef struct {
    Group *arr;
    size_t len, cap;
} Groups;

typedef struct {
    Segment *arr; size_t len, cap;
} SegVec;

/* ----------- Groups structure helpers ----------- */
static int double_comp_eq(double a, double b, double tol){
    return fabs(a-b)<tol?1:0;
}
static void groups_init(Groups *g){ g->arr=NULL; g->len=0; g->cap=0; }
static void segments_reserve(Group *g, size_t need){
    if (g->cap >= need) return;
    size_t ncap = g->cap ? g->cap*2 : 16;
    if (ncap < need) ncap = need;
    Segment *na = (Segment*)realloc(g->arr, ncap*sizeof(Segment));
    g->arr = na; g->cap = ncap;
}
static void groups_reserve(Groups *gs, size_t need){
    if (gs->cap >= need) return;
    size_t ncap = gs->cap ? gs->cap*2 : 16;
    if (ncap < need) ncap = need;
    Group *na = (Group*)realloc(gs->arr, ncap*sizeof(Group));
    gs->arr = na; gs->cap = ncap;
}

static ssize_t groups_find_key(Groups *gs, double key){
    // keys are quantized; equality match is fine
    for(size_t i=0;i<gs->len;i++){
        if (double_comp_eq(gs->arr[i].key,key,global_tol)) return (ssize_t)i;
    }
    return -1;
}
static size_t groups_get_or_add(Groups *gs, double key){
    ssize_t idx = groups_find_key(gs, key);
    if (idx>=0) return (size_t)idx;
    groups_reserve(gs, gs->len+1);
    Group g = {0};
    g.key = key; g.arr=NULL; g.len=0; g.cap=0;
    gs->arr[gs->len] = g;
    return gs->len++;
}

/* ----------- G-code parsing helpers ----------- */

static int starts_with_ci(const char *s, const char *prefix){
    while(*prefix){
        if (toupper((unsigned char)*s++) != toupper((unsigned char)*prefix++)) return 0;
    }
    return 1;
}

// Extract first occurrence of e.g. 'X' number from a line; returns 1 if found.
static int extract_word_double_ci(const char *line, char letter, double *out){
    const char *p = line;
    while (*p){
        if (toupper((unsigned char)*p) == toupper((unsigned char)letter)){
            // move past letter, optional spaces, then parse number
            p++;
            while(*p==' ' || *p=='\t') p++;
            char *endptr=NULL;
            double v = strtod(p, &endptr);
            if (endptr!=p){ *out = v; return 1; }
        }
        p++;
    }
    return 0;
}

// Identify main command at start: G0/G00/G1/G01/M03/M3/M05/M5 ...
static int classify_line(const char *s, char *outType){
    // Skip leading spaces and comments (we keep comments as OTHER)
    while(*s==' '||*s=='\t') s++;
    if (*s=='(') { *outType='C'; return 1; } // comment
    if (*s==0)   { *outType='E'; return 1; } // empty

    if (starts_with_ci(s,"G0 ")) { *outType='M'; return 1; }
    if (starts_with_ci(s,"G00")) { *outType='M'; return 1; }
    if (starts_with_ci(s,"G1 ")) { *outType='M'; return 1; }
    if (starts_with_ci(s,"G01")) { *outType='M'; return 1; }
    if (starts_with_ci(s,"M03")) { *outType='3'; return 1; }
    if (starts_with_ci(s,"M3"))  { *outType='3'; return 1; }
    if (starts_with_ci(s,"M05")) { *outType='5'; return 1; }
    if (starts_with_ci(s,"M5"))  { *outType='5'; return 1; }
    *outType='O'; return 1; // other
}

static void segvec_push(SegVec *v, Segment s){
    if (v->cap <= v->len){
        size_t ncap = v->cap ? v->cap*2 : 128;
        Segment *na = (Segment*)realloc(v->arr, ncap*sizeof(Segment));
        v->arr = na; v->cap = ncap;
    }
    v->arr[v->len++] = s;
}

static void collect_segments(FILE *fp, SegVec *out, double *out_first_feed, int *has_first_feed){
    char *line = NULL;
    size_t n = 0;
    ssize_t r;
    double x=0.0, y=0.0;
    int laser_on = 0;
    *has_first_feed = 0;

    while ((r = getline(&line, &n, fp)) != -1){
        (void)r;
        char type;
        classify_line(line, &type);

        if (type=='3'){ laser_on = 1; continue; }
        if (type=='5'){ laser_on = 0; continue; }
        if (type=='M'){ // Move (G0/G1)
            // parse X/Y/F if present
            double nx=x, ny=y, nf, ns;
            int hx = extract_word_double_ci(line,'X', &nx);
            int hy = extract_word_double_ci(line,'Y', &ny);
            int hf = extract_word_double_ci(line,'F', &nf);
            int hs = extract_word_double_ci(line,'S', &ns);
            if (hf && !*has_first_feed){
                *out_first_feed = nf;
                *has_first_feed = 1;
                
            }

            int is_g1 = starts_with_ci(line,"G1 ") || starts_with_ci(line,"G01");
            if (is_g1 && laser_on && (hx || hy)){
                    Segment s;
                    s.x1 = x; s.y1 = y; s.x2 = nx; s.y2 = ny;
                    s.has_power = hs;                              
                    s.power = hs ? ns : 0.0;
                    segvec_push(out, s);
            }
            if (hx){ x = nx;}
            if (hy){ y = ny;}
        }
    }
    free(line);
}

static void args_usage(){
    printf("Usage:\n"
        "<input.gcode>\n"
        "[--overscan <mm>] [--power <S>] [--tolerance <mm>] [--feed <F>\n");
    return;
}

static int parse_args (int argc, char **argv, Options *opt){
    opt->in_path = NULL;
    opt->overscan = 2.0;
    opt->power = 75.0;
    opt->tol = 0.0001;
    opt->feed = 1000.0;
    if (argc < 2) {args_usage(); return 0; }
    for (int i=1;i<argc;i++){
        const char *a = argv[i];
        if (a[0] != '-'){
            opt->in_path = a; 
            char *dot = strrchr(opt->in_path, '.'); 
            if (dot){
                strncpy(opt->out_path, opt->in_path, dot-opt->in_path);
                opt->out_path[dot-opt->in_path]='\0';
                char ext[10];
                strcpy(ext,dot);
                strcat(opt->out_path,"_overscan");
                strcat(opt->out_path,ext);
            }
            else { 
                strcpy(opt->out_path,opt->in_path);
                strcat(opt->out_path,"_overscan");
            }
            continue; 
        }
        if (strcmp(a,"--overscan")==0 && i+1<argc){ opt->overscan = atof(argv[++i]); continue; }
        if (strcmp(a,"--power")==0    && i+1<argc){ opt->power = atof(argv[++i]); continue; }
        if (strcmp(a,"--tolerance")==0 && i+1<argc){ opt->tol = atof(argv[++i]); continue; }
        if (strcmp(a,"--feed")==0&& i+1<argc){ opt->feed = atof(argv[++i]); continue; }

    }
    if (!opt->in_path){ args_usage(); return 0;}
    return 1;
}

static int is_vertical_seg(const Segment *s, double tol){
    return fabs(s->x2 - s->x1) <= tol && fabs(s->y2 - s->y1) > tol;
}
static int is_horizontal_seg(const Segment *s, double tol){
    return fabs(s->y2 - s->y1) <= tol && fabs(s->x2 - s->x1) > tol;
}

static double quantize(double v, double tol){
    if (tol <= 0.0) return v;
    double q = round(v / tol) * tol;
    return q;
}

static int cmp_vrt_segment(const void *pa, const void *pb){
    const Segment *a=(const Segment*)pa, *b=(const Segment*)pb;
    if (a->y1 < b->y1) return -1;
    if (a->y1 > b->y1) return 1;
    return 0;
}

static int cmp_hrz_segment(const void *pa, const void *pb){
    const Segment *a=(const Segment*)pa, *b=(const Segment*)pb;
    if (a->x1 < b->x1) return -1;
    if (a->x1 > b->x1) return  1;
    return 0;
}

static int cmp_grp(const void *pa, const void *pb){
    const Group *a=(const Group*)pa, *b=(const Group*)pb;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return  1;
    return 0;
}

static void group_segments(const SegVec *segs, Groups *vg, Groups *hg, double tol){
    for(size_t i=0;i<segs->len;i++){
        const Segment *s = &segs->arr[i];
        if (is_vertical_seg(s, tol)){
            double xmid = 0.5*(s->x1 + s->x2);
            double xk = quantize(xmid, tol);
            double y1 = s->y1, y2 = s->y2;
            if (y1>y2){ double t=y1; y1=y2; y2=t; }
            size_t gi = groups_get_or_add(vg, xk);
            Group *g = &vg->arr[gi];
            segments_reserve(g, g->len+1);
            g->arr[g->len++] = (Segment){ .x1=s->x1, .x2=s->x2, .y1=y1, .y2=y2, .power=s->power, .has_power=s->has_power};
        } else if (is_horizontal_seg(s, tol)){
            double ymid = 0.5*(s->y1 + s->y2);
            double yk = quantize(ymid, tol);
            double x1 = s->x1, x2 = s->x2;
            if (x1>x2){ double t=x1; x1=x2; x2=t; }
            size_t gi = groups_get_or_add(hg, yk);
            Group *g = &hg->arr[gi];
            segments_reserve(g, g->len+1);
            g->arr[g->len++] = (Segment){ .x1=x1, .x2=x2, .y1=s->y1, .y2=s->y2, .power=s->power, .has_power=s->has_power};;
        } else {
            // ignore diagonals
        }
    }
    // sort segments inside groups
    for(size_t i=0;i<vg->len;i++){
        Group *g = &vg->arr[i];
        qsort(g->arr, g->len, sizeof(Segment), cmp_vrt_segment);
    }
    for(size_t i=0;i<hg->len;i++){
        Group *g = &hg->arr[i];
        qsort(g->arr, g->len, sizeof(Segment), cmp_hrz_segment);
    }
    //sort groups by key
    qsort(vg->arr,vg->len,sizeof(Group),cmp_grp);
    qsort(hg->arr,hg->len,sizeof(Group),cmp_grp);
}

/* ----------- Output helpers ----------- */

static void emit_header(FILE *fo, double cut_feed){
    fprintf(fo, "(Generated by overscan_clang)\n");
    fprintf(fo, "G21\nG90\nG94\n");
    fprintf(fo, "G01 F%.2f\n\n\n", cut_feed);
}
static void g0(FILE *fo, double x, double y){
    fprintf(fo, "G00 X%.4f Y%.4f S0\n", x, y);
}
static void g1(FILE *fo, double x, double y, double power){
    fprintf(fo, "G01 X%.4f Y%.4f S%.4f\n", x, y, power);
}
static void m3(FILE *fo, double power){ fprintf(fo, "M03 S%.3f\n\n\n", power); }
static void m5(FILE *fo){ fprintf(fo, "M05\n\n\n"); }

/* ----------- Planning passes ----------- */

static void plan_vertical(FILE *fo, const Groups *vg, double overscan, double power)
{
    int dir_up = 1;
    for(size_t i=0;i<vg->len;i++){
        const Group *g = &vg->arr[i];
        if (g->len>0){
            fprintf(fo, "(V column X=%.4f direction: %s)\n", g->key, dir_up?"up":"down");
            if (dir_up){
                g0(fo, g->arr[0].x1, g->arr[0].y1-overscan);
                for(size_t k=0;k<g->len;k++){
                    g0(fo, g->arr[k].x1, g->arr[k].y1);
                    g1(fo, g->arr[k].x2, g->arr[k].y2, power);
                }
                g0(fo, g->arr[g->len-1].x2, g->arr[g->len-1].y2+overscan);
            } else {
                g0(fo, g->arr[g->len-1].x2, g->arr[g->len-1].y2+overscan);
                for(ssize_t k=(ssize_t)g->len-1;k>=0;k--){
                    g0(fo, g->arr[k].x2, g->arr[k].y2);
                    g1(fo, g->arr[k].x1, g->arr[k].y1, power);
                }
                g0(fo, g->arr[0].x1, g->arr[0].y1-overscan);
            }
            dir_up = !dir_up;
        }
    }
}

static void plan_horizontal(FILE *fo, const Groups *hg, double overscan, double power)
{
    int dir_right = 1;
    for(size_t i=0;i<hg->len;i++){
        const Group *g = &hg->arr[i];
        if (g->len>0){
            fprintf(fo, "(H row Y=%.4f direction: %s)\n", g->key, dir_right?"right":"left");
            if (dir_right){
                g0(fo, g->arr[0].x1-overscan, g->arr[0].y1);
                for(size_t k=0;k<g->len;k++){
                    g0(fo, g->arr[k].x1, g->arr[k].y1);
                    g1(fo, g->arr[k].x2, g->arr[k].y2, power);
                }
                g0(fo, g->arr[g->len-1].x2+overscan, g->arr[g->len-1].y2);
            } else {
                g0(fo, g->arr[g->len-1].x2+overscan, g->arr[g->len-1].y2);
                for(ssize_t k=(ssize_t)g->len-1;k>=0;k--){
                    g0(fo, g->arr[k].x2, g->arr[k].y2);
                    g1(fo, g->arr[k].x1, g->arr[k].y1, power);
                }
                g0(fo, g->arr[0].x1-overscan, g->arr[0].y1);
            }
            dir_right = !dir_right;
        }
    }
}

int main(int argc, char **argv){
    Options opt;
    if (!parse_args(argc, argv, &opt)) return 1;

    FILE *fi = fopen(opt.in_path, "r");
    if (!fi){ printf("Cannot open input: %s (%s)\n", opt.in_path); return 1; }

    SegVec segs = {0};
    double first_feed=0.0; int has_first_feed=0;
    collect_segments(fi, &segs, &first_feed, &has_first_feed);
    fclose(fi);

    Groups vgroups; // key = X
    Groups hgroups; // key = Y
    groups_init(&vgroups);
    groups_init(&hgroups);
   
    group_segments(&segs, &vgroups, &hgroups, opt.tol);

    FILE *fo = fopen(opt.out_path, "w");
    if (!fo){ printf("Cannot open output: %s (%s)\n", opt.out_path); return 1; }
    emit_header(fo, opt.feed);
    m3(fo, opt.power);
    plan_vertical(fo, &vgroups, opt.overscan, opt.power);
    plan_horizontal(fo, &hgroups, opt.overscan, opt.power);
    m5(fo);
    fclose(fo);

    // Cleanup  
    for(size_t i=0;i<vgroups.len;i++) free(vgroups.arr[i].arr);
    for(size_t i=0;i<hgroups.len;i++) free(hgroups.arr[i].arr);
    free(vgroups.arr); free(hgroups.arr);
    free(segs.arr);


    return 0;
}