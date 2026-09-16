#include "hpx_mirror.h"
#include <math.h>
#include <stdio.h>

#define CELL_COUNT 1000000u
#define NDIR 6
static const int DIR_STEP[NDIR] = {-2, 2, -1, 1, -4, 4};
static const int DIR_RECV[NDIR] = {0, 1, 3, 2, 4, 5};
static const int CH_DIM[NDIR] = {0, 0, 1, 1, 2, 2};
static const int CH_DIR[NDIR] = {1, -1, 1, -1, 1, -1};
enum { MEMBERS, CHILD, SCELL, LIST1, LIST2, NLIST };
enum { P_N, P_NT, P_SIZE, P_THETA, P_TH, P_KTH, P_NP, P_NL, P_RANK,
       P_RANGE, P_BODY, P_DIR, P_TREE_TPL, P_ENUM_TPL, P_LIST_TPL, P_GRAPH_TPL,
       P_STAGE_TPL, P_REAP_TPL, P_CLEAN_TPL, P_COLLECT_TPL, P_PULL_TPL,
       P_REPORT, P_START_EVENT, P_CLOSE, P_OP, P_T, P_J, P_D, P_FIRST,
       P_TREE_DEPS, P_POOL, P_ALLOC_TPL, P_COUNT };
enum { NM1, NM2, CP1, CP2, CM1, CM2, CH1, CH2, NB1, NB2 };

typedef struct { int ID1, parent, m1; double r1[3], v1[3], force[3]; } body_t;
typedef struct {
  u32 count[NLIST];
  int ID2, parent2, NumNodes, level, neighbors[6];
  double m2, r2[3], rd[3], boundary[6];
} cell_t;
typedef struct { ocrGuid_t db[NLIST + 1]; u32 slot[NLIST + 1]; } cell_ref_t;
typedef struct { u32 n, cap; int *data; } ints_t;
typedef struct { atomic_flag mutex; u64 count, capacity; ocrGuid_t free[]; } pool_t;
typedef struct {
  u64 *pv; body_t *b; cell_ref_t *dir; ocrEdtDep_t *deps; u32 base;
} tree_view_t;

static void push(ints_t *v, int x) {
  if (v->n == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 4;
    v->data = realloc(v->data, (size_t)v->cap * sizeof(int));
    if (!v->data) abort();
  }
  v->data[v->n++] = x;
}
static ocrGuid_t new_db(u64 *pv, u64 bytes, void **ptr) {
  ocrHint_t h; mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_DB_T);
  ocrGuid_t g;
  ocrDbCreate(&g, ptr, bytes ? bytes : 1, DB_PROP_NONE, &h, NO_ALLOC);
  return g;
}
static void pool_lock(pool_t *pool) {
  while (atomic_flag_test_and_set_explicit(&pool->mutex, memory_order_acquire)) {}
}
static ocrGuid_t pool_take(u64 *pv, pool_t *pool, void **created) {
  pool_lock(pool);
  ocrGuid_t db;
  *created = NULL;
  if (pool->count) db = pool->free[--pool->count];
  else db = new_db(pv, pv[P_SIZE] * sizeof(body_t), created);
  atomic_flag_clear_explicit(&pool->mutex, memory_order_release);
  return db;
}
static ocrGuid_t allocate_edt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t d[]) {
  (void)pc; (void)dc;
  void *created;
  ocrGuid_t db = pool_take(pv, d[2].ptr, &created);
  if (created) ocrDbRelease(db);
  return db;
}
static ocrGuid_t new_event(void) {
  ocrGuid_t g; ocrEventCreate(&g, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG); return g;
}
static ocrGuid_t make_task(u64 *pv, u64 tpl, u32 n, ocrGuid_t *ready) {
  ocrHint_t h; mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_EDT_T);
  ocrGuid_t task, out;
  ocrEdtCreate(&task, mirror_u64_guid(tpl), P_COUNT, pv, n, NULL,
               EDT_PROP_NONE, &h, ready ? &out : NULL);
  if (ready) {
    *ready = new_event();
    ocrAddDependence(out, *ready, 0, DB_MODE_NULL);
  }
  return task;
}
static const cell_t *cell_at(tree_view_t *v, int i) {
  static const cell_t empty = {0};
  if (i < 0 || (u32)i >= CELL_COUNT || ocrGuidIsNull(v->dir[i].db[0])) return &empty;
  return v->deps[v->base + v->dir[i].slot[0]].ptr;
}
static const int *list_at(tree_view_t *v, int i, int kind) {
  return v->deps[v->base + v->dir[i].slot[kind + 1]].ptr;
}
static int inside(const body_t *b, int node, const cell_t *c) {
  for (int i=0; i<3; ++i)
    if (b[node].r1[i] < c->boundary[2*i] || b[node].r1[i] > c->boundary[2*i+1]) return 0;
  return 1;
}
static void mass(cell_t *c, const int *members, const body_t *b) {
  volatile float cm[3]={0,0,0}; c->m2=0;
  for (u32 j=0; j<c->count[MEMBERS]; ++j) {
    c->m2 += b[members[j]].m1;
    for(int i=0;i<3;++i) cm[i] = cm[i] + b[members[j]].r1[i]*b[members[j]].m1;
  }
  for(int i=0;i<3;++i) {c->r2[i]=cm[i]/c->m2;c->rd[i]=c->boundary[2*i+1]-c->boundary[2*i];}
}
static void bounds(const cell_t *p, cell_t *c, int j) {
  static const int order[8]={1,3,5,7,0,2,4,6};
  int box=order[j];
  for(int a=0;a<3;++a) {
    double mid=(p->boundary[2*a]+p->boundary[2*a+1])/2;
    int high=(box >> (2-a))&1;
    c->boundary[2*a]=high?mid:p->boundary[2*a];
    c->boundary[2*a+1]=high?p->boundary[2*a+1]:mid;
  }
  for(int a=0;a<6;++a)c->neighbors[a]=-1;
}
static int create_children(u64 *pv,const cell_t *parent,cell_ref_t *dir) {
  u64 first=(u64)parent->ID2*8+1;
  if(first+7>=CELL_COUNT){PRINTF("nbody_hpx: tree exceeds cell array\n");ocrShutdown();return 0;}
  for(int i=0;i<8;++i) {
    cell_t *child;
    ocrGuid_t db=dir[first+i].db[0]=new_db(pv,sizeof(*child),(void**)&child);
    memset(child,0,sizeof(*child));bounds(parent,child,i);ocrDbRelease(db);
  }
  return 1;
}
static void store_list(u64 *pv, cell_ref_t *r, cell_t *c, int kind, ints_t *v) {
  c->count[kind]=v->n;
  if(v->n) {
    int *p; r->db[kind+1]=new_db(pv, (u64)v->n*sizeof(int), (void**)&p);
    memcpy(p,v->data,(size_t)v->n*sizeof(int));ocrDbRelease(r->db[kind+1]);
  }
  free(v->data);
}
static void wire_tree(ocrGuid_t task, u32 base, cell_ref_t *dir, ocrDbAccessMode_t mode) {
  for(u32 i=0;i<CELL_COUNT;++i) for(int k=0;k<=NLIST;++k)
    if(!ocrGuidIsNull(dir[i].db[k]))
      ocrAddDependence(dir[i].db[k],task,base+dir[i].slot[k], k==0?mode:DB_MODE_RO);
}
static ocrGuid_t subtree_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;
  body_t *b=d[0].ptr; cell_ref_t *dir=d[1].ptr; cell_t *parent=d[2].ptr;
  const int *members=d[3].ptr;
  int m=parent->ID2;u64 first=(u64)m*8+1;
  if(first+7>=CELL_COUNT){PRINTF("nbody_hpx: tree exceeds cell array\n");ocrShutdown();return NULL_GUID;}
  ints_t children={0},subcells={0};
  ocrGuid_t child_db[8],member_db[8];u32 counts[8];
  for(int i=0;i<8;++i) {
    cell_t *c=d[4+i].ptr;cell_ref_t *r=&dir[first+i];
    child_db[i]=r->db[0];
    c->ID2=(int)(first+i);c->parent2=m;c->level=parent->level+1;
    ints_t ids={0};
    for(u32 j=0;j<parent->count[MEMBERS];++j) if(inside(b,members[j],c)) {
      b[members[j]].parent=c->ID2;push(&ids,b[members[j]].ID1);++c->NumNodes;
    }
    c->count[MEMBERS]=ids.n;
    if(ids.n>1)mass(c,ids.data,b);
    if(ids.n && c->NumNodes<=(int)pv[P_TH]) for(int j=0;j<c->NumNodes;++j) {
      b[b[ids.data[j]].ID1].parent=m;push(&children,b[ids.data[j]].ID1);
    }
    if(ids.n>pv[P_TH]){push(&subcells,c->ID2);if(!create_children(pv,c,dir))return NULL_GUID;}
    counts[i]=ids.n;store_list(pv,r,c,MEMBERS,&ids);member_db[i]=r->db[MEMBERS+1];
    ocrDbRelease(child_db[i]);
  }
  store_list(pv,&dir[m],parent,CHILD,&children);
  store_list(pv,&dir[m],parent,SCELL,&subcells);
  ocrHint_t h;mirror_rank_hint(&h,pv[P_RANK],OCR_HINT_EDT_T);
  for(int i=0;i<8;++i)if(counts[i]>pv[P_TH]) {
    ocrGuid_t child;
    ocrEdtCreate(&child,mirror_u64_guid(pv[P_TREE_TPL]),P_COUNT,pv,12,NULL,EDT_PROP_FINISH,&h,NULL);
    ocrAddDependence(mirror_u64_guid(pv[P_BODY]),child,0,DB_MODE_RW);
    ocrAddDependence(mirror_u64_guid(pv[P_DIR]),child,1,DB_MODE_RW);
    ocrAddDependence(child_db[i],child,2,DB_MODE_RW);
    ocrAddDependence(member_db[i],child,3,DB_MODE_RO);
    for(int k=0;k<8;++k)ocrAddDependence(dir[(first+i)*8+1+k].db[0],child,4+k,DB_MODE_RW);
  }
  return NULL_GUID;
}
static void traverse(tree_view_t *v,int rt,int node,int root,ints_t *l1,ints_t *l2) {
  const cell_t *c=cell_at(v,root);
  float dist=(float)sqrt(pow(v->b[node].r1[0]-c->r2[0],2.0)+pow(v->b[node].r1[1]-c->r2[1],2.0)+pow(v->b[node].r1[2]-c->r2[2],2.0));
  float radius=(float)sqrt(pow(c->rd[0],2.0)+pow(c->rd[1],2.0)+pow(c->rd[2],2.0));
  double theta;memcpy(&theta,&v->pv[P_THETA],sizeof theta);
  float ratio=dist/radius;
  u32 target=v->dir[rt].slot[0];
  if(ratio<theta) {
    if(c->count[CHILD]){const int *ids=list_at(v,root,CHILD);for(u32 i=0;i<c->count[CHILD];++i)push(&l1[target],ids[i]);}
    if(c->count[SCELL]){const int *ids=list_at(v,root,SCELL);for(u32 i=0;i<c->count[SCELL];++i)traverse(v,rt,node,ids[i],l1,l2);}
  } else push(&l2[target],root);
}
static ocrGuid_t list_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;tree_view_t v={pv,d[0].ptr,d[1].ptr,d,2};
  ints_t *l1=calloc(dc,sizeof(*l1)),*l2=calloc(dc,sizeof(*l2));if(!l1||!l2)abort();
  for(u64 i=0;i+1<pv[P_N];++i)if(cell_at(&v,(int)i)->count[CHILD])traverse(&v,v.b[i].parent,(int)i,0,l1,l2);
  for(u32 i=0;i<CELL_COUNT;++i)if(!ocrGuidIsNull(v.dir[i].db[0])) {
    u32 slot=v.dir[i].slot[0];cell_t *c=d[2+slot].ptr;
    store_list(pv,&v.dir[i],c,LIST1,&l1[slot]);store_list(pv,&v.dir[i],c,LIST2,&l2[slot]);
  }
  free(l1);free(l2);return NULL_GUID;
}

static u64 neighbour_index(u64 i, int dir, u64 size) {
  if (dir == +1 || dir == -1) {
    if (i == 0 && dir == -1) return size - 1;
    if (i == size - 1 && dir == +1) return 0;
    return i + dir;
  }
  if (dir == +2 || dir == -2) {
    if (size == 8 || size == 16 || size == 32) return size - 1 - i;
    if (size == 64) {
      if (dir == -2 && i < 4) return size - (i + 1);
      if (dir == +2 && i > size - 5) return size - 1 - i;
    }
    /* The fall-through is signed where the table is, so a step below zero
     * stays the answer that table gives rather than becoming a large one. */
    long long temp = (long long)i;
    if (size == 64) temp = (long long)i + (2 * dir);
    if (size < 8) temp = (long long)i;
    return (u64)temp;
  }
  /* dir == +4 || dir == -4 */
  if (size == 4) {
    if (dir == -4 && i < 2) return size - (i + 1);
    if (dir == +4 && i > size - 3) return size - 1 - i;
  }
  if (size == 8) {
    if (dir == -4 && i < 4) return size - (i + 1);
    if (dir == +4 && i > size - 5) return size - 1 - i;
  }
  if (size == 16 || size == 32) {
    if (dir == -4 && i < 8) return size - (i + 1);
    if (dir == +4 && i > size - 9) return size - 1 - i;
  }
  if (size == 64) {
    if (dir == -4 && i < 16) return size - (i + 1);
    if (dir == +4 && i > size - 17) return size - 1 - i;
  }
  long long temp = (long long)i;
  if (size == 4) temp = (long long)i + (int)(dir * 0.5);
  if (size == 8) temp = (long long)i + dir;
  if (size == 16 || size == 32) temp = (long long)i + (dir * 2);
  if (size == 64) temp = (long long)i + (dir * 4);
  if (size < 4) temp = (long long)i;
  return (u64)temp;
}

/* Which cells of the level a rank owns. */
static void owned_cells(u64 ID, u64 np, u64 nl, int *out, u64 *count) {
  u64 local_np = np / nl;
  *count = local_np;
  if (np == 8) {
    if (nl != 1)
      for (u64 i = 0; i < local_np; ++i) out[i] = (int)(1 + i + (8 / nl) * ID);
    else
      for (u64 i = 0; i < local_np; ++i) out[i] = (int)(ID + 1 + i);
  } else {
    for (u64 i = 0; i < local_np; ++i) out[i] = (int)(9 + i + (64 / nl) * ID);
  }
}

static double *compute_r(tree_view_t *v, int node) {
  /* The intermediate assignment rounds to float before the position update. */
  volatile float a[3] = { 0.0f, 0.0f, 0.0f };
  int parent = v->b[node].parent;
  const cell_t *pc = cell_at(v,parent);
  const int *ids1=pc->count[LIST1]?list_at(v,parent,LIST1):NULL;
  const int *ids2=pc->count[LIST2]?list_at(v,parent,LIST2):NULL;
  double grav=6.673*pow(10.0,-11.0);

  if (pc->count[LIST1] >= 1)
    for (int j = 0; j < 3; ++j)
      for (u32 i = 0; i < pc->count[LIST1]; ++i)
        v->b[node].force[j] = 1 + v->b[node].force[j]
          + (grav * v->b[node].m1 * v->b[ids1[i]].m1)
            * (v->b[node].r1[j] - v->b[ids1[i]].r1[j])
            / pow((1 + pow((v->b[node].r1[0] - v->b[ids1[i]].r1[0]), 2.0)
                     + pow((v->b[node].r1[1] - v->b[ids1[i]].r1[1]), 2)
                     + pow((v->b[node].r1[2] - v->b[ids1[i]].r1[2]), 2.0)), 1.5);

  if (pc->count[LIST2] >= 1)
    for (int j = 0; j < 3; ++j)
      for (u32 i = 0; i < pc->count[LIST2]; ++i)
        v->b[node].force[j] = 1 + v->b[node].force[j]
          + (grav * v->b[node].m1 * cell_at(v,ids2[i])->m2)
            * (v->b[node].r1[j] - cell_at(v,ids2[i])->r2[j])
            / pow((1 + pow((v->b[node].r1[0] - cell_at(v,ids2[i])->r2[0]), 2.0)
                     + pow((v->b[node].r1[1] - cell_at(v,ids2[i])->r2[1]), 2.0)
                     + pow((v->b[node].r1[2] - cell_at(v,ids2[i])->r2[2]), 2.0)), 1.5);

  for (int j = 0; j < 3; ++j) {
    a[j] = v->b[node].force[j] / v->b[node].m1;
    v->b[node].r1[j] = v->b[node].r1[j] + v->b[node].v1[j] * 1
                    + 0.5 * a[j] * 1 * 1;
  }
  return v->b[node].force;
}

static void new_tree(tree_view_t *v,int node,int parent) {
  const cell_t *c=cell_at(v,parent);
  for(int j=0;j<6;++j)if(c->neighbors[j]!=-1) {
    /* The reachable tree has no populated neighbor links. */
    PRINTF("nbody_hpx: unexpected populated neighbor table\n");ocrShutdown();return;
  }
  if(c->ID2!=0)new_tree(v,node,c->parent2);
}
static ocrGuid_t stage_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;
  u64 size=pv[P_SIZE],kth=pv[P_KTH];int op=(int)pv[P_OP];
  body_t *next;ocrGuid_t db;
  if(!(op&1)){db=d[dc-1].guid;next=d[dc-1].ptr;}
  else {db=d[0].guid;next=d[0].ptr;}
  if(op==NM1 || op==CM1 || op==NB1) {
    const body_t *m=d[0].ptr;for(u64 i=0;i<size;++i)next[i]=m[i];
  } else if(op==NM2) {
    const body_t *l=d[1].ptr,*b=d[2].ptr;const cell_t *cell=d[3].ptr;u64 k=0;
    for(u64 i=0;i<size;++i)if(l[i].ID1>=0 && k<kth)
      if(inside(b,l[i].ID1,cell)){next[i]=l[i];++k;}
  } else if(op==CP1) {
    const body_t *m=d[0].ptr;tree_view_t v={pv,d[2].ptr,d[3].ptr,d,4};
    for(u64 i=0;i<size;++i) {
      next[i]=m[i];next[i].ID1=m[i].ID1;
      if(next[i].ID1>0) {
        double *f=compute_r(&v,next[i].ID1);
        next[i].force[0]=f[0];next[i].force[1]=f[1];next[i].force[2]=f[2];
        v.b[next[i].ID1].force[0]=next[i].force[0];
        v.b[next[i].ID1].force[1]=next[i].force[1];v.b[next[i].ID1].force[2]=next[i].force[2];
      }
    }
  } else if(op==CP2) {
    u64 k=0;
    for(int dir=0;dir<NDIR;++dir) {
      const body_t *l=d[2+dir].ptr;
      for(u64 i=0;i<size;++i)if(k<kth){next[size-kth+k]=l[i];++k;}
    }
    tree_view_t v={pv,d[8].ptr,d[9].ptr,d,10};
    for(u64 i=size-kth;i<size;++i)if(next[i].ID1>0)new_tree(&v,next[i].ID1,next[i].parent);
  } else if(op==CH1) {
    const body_t *m=d[0].ptr;const cell_t *cell=d[2].ptr;
    int dim=CH_DIM[pv[P_D]],dir=CH_DIR[pv[P_D]];
    for(u64 i=0;i<size;++i) {
      next[i]=m[i];next[i].ID1=-1;
      if(m[i].ID1>=0) {
        if(dir==-1 && m[i].r1[dim]>cell->boundary[2*dim])next[i].ID1=m[i].ID1;
        if(dir==1)next[i].ID1=m[i].ID1;
      }
    }
  } else if(op==CH2) {
    const body_t *m=d[1].ptr,*l=d[2].ptr;const cell_t *cell=d[3].ptr;
    int dim=CH_DIM[pv[P_D]],dir=CH_DIR[pv[P_D]];u64 k=0;
    for(u64 i=0;i<size;++i)if(l[i].ID1>0 && k<kth) {
      if(dir==-1 && m[i].r1[dim]<cell->boundary[2*dim]){next[size-kth+k]=m[i];++k;}
      if(dir==1 && m[i].r1[dim]>cell->boundary[2*dim+1]){next[size-kth+k]=m[i];++k;}
    }
  } else if(op==NB2) {
    const body_t *l=d[2].ptr;int k=0;
    for(u64 i=0;i<size;++i)if(next[i].ID1<0 && !k) {
      k=1;
      if(i+size<size)for(u64 j=0;j<size;++j)next[i+j]=l[j];
      else if(i+size>size)for(u64 j=i;j<size;++j)next[j]=l[j-i];
    }
  }
  if(op&1)ocrEventDestroy(mirror_u64_guid(pv[P_FIRST]));
  ocrDbRelease(db);return db;
}

typedef struct {ocrGuid_t value;ocrGuid_t *users;u32 count,cap;} value_t;
typedef struct {value_t *values;u32 count,cap;ocrEdtDep_t *tree;u32 tree_count;} graph_t;
static void guid_push(ocrGuid_t **p,u32 *n,u32 *cap,ocrGuid_t x) {
  if(*n==*cap){*cap=*cap?2**cap:16;*p=realloc(*p,(size_t)*cap*sizeof(**p));if(!*p)abort();}
  (*p)[(*n)++]=x;
}
static u32 value_new(graph_t *g,ocrGuid_t value) {
  if(g->count==g->cap){g->cap=g->cap?g->cap*2:64;g->values=realloc(g->values,(size_t)g->cap*sizeof(*g->values));if(!g->values)abort();}
  u32 i=g->count++;g->values[i]=(value_t){.value=value};return i;
}
static void use_value(graph_t *g,u32 id,ocrGuid_t end) {
  value_t *v=&g->values[id];guid_push(&v->users,&v->count,&v->cap,end);
}
static ocrGuid_t point(u64 *pv,u64 t,int dir,u64 rank,int ack) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]),t*NDIR+dir+(ack?pv[P_NT]*NDIR:0),rank,pv[P_NL]);
}
static ocrGuid_t gather_point(u64 *pv,u64 rank) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]),2*pv[P_NT]*NDIR,rank,pv[P_NL]);
}
static ocrGuid_t cleanup_point(u64 *pv,u64 rank) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]),2*pv[P_NT]*NDIR+1,rank,pv[P_NL]);
}
static u64 sender(u64 *pv,int recv,int *send_dir) {
  for(int d=0;d<NDIR;++d)if(DIR_RECV[d]==recv) {
    *send_dir=d;
    for(u64 r=0;r<pv[P_NL];++r)if(neighbour_index(r,DIR_STEP[d],pv[P_NL])==pv[P_RANK])return r;
  }
  abort();
}
static void notify_value(u64 *pv,graph_t *g,u32 value,u64 t) {
  for(int d=0;d<NDIR;++d) {
    u64 to=neighbour_index(pv[P_RANK],DIR_STEP[d],pv[P_NL]);
    ocrGuid_t arrival=point(pv,t,DIR_RECV[d],to,0),ack=point(pv,t,d,pv[P_RANK],1);
    mirror_edge_open(arrival);mirror_edge_open(ack);
    use_value(g,value,ack);
    ocrAddDependence(g->values[value].value,arrival,0,DB_MODE_NULL);
  }
}
static u32 operation(u64 *pv,graph_t *g,cell_ref_t *dir,int op,u32 a,u32 b,ocrGuid_t arrived,int cellid) {
  u64 q[P_COUNT];memcpy(q,pv,sizeof q);q[P_OP]=op;
  u32 tree=(u32)pv[P_TREE_DEPS];
  u32 n1=op==NM1?2:op==CP1?4+tree:op==CH1?3:op==NB1?2:1;
  u32 n2=op==NM1?4:op==CP1?10+tree:op==CH1?4:op==NB1?3:1;
  ocrGuid_t first,last;
  ocrGuid_t t1=make_task(q,pv[P_STAGE_TPL],n1+1,&first);
  ocrGuid_t allocation, allocated;
  ocrHint_t h; mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_EDT_T);
  ocrEdtCreate(&allocation, mirror_u64_guid(pv[P_ALLOC_TPL]), P_COUNT, q,
               3, NULL, EDT_PROP_NONE, &h, &allocated);
  ocrAddDependence(allocated, t1, n1, DB_MODE_RW);
  q[P_OP]=op+1;q[P_FIRST]=mirror_guid_u64(first);
  ocrGuid_t t2=make_task(q,pv[P_STAGE_TPL],n2,&last);
  ocrGuid_t av=g->values[a].value,bv=op==CM1?NULL_GUID:g->values[b].value;
  use_value(g,a,last);if(op!=CM1 && b!=a)use_value(g,b,last);
  ocrAddDependence(op==NM1?arrived:av, allocation, 0, DB_MODE_NULL);
  ocrAddDependence(op==NM1?av:bv, allocation, 1, DB_MODE_NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_POOL]), allocation, 2, DB_MODE_RW);

  u32 result=value_new(g,last);
  if(op==NM1) {
    int send_dir;u64 from=sender(pv,(int)pv[P_D],&send_dir);
    ocrGuid_t ack=point(pv,pv[P_T],send_dir,from,1);mirror_edge_open(ack);
    ocrAddDependence(last,ack,0,DB_MODE_NULL);
    ocrAddDependence(arrived,t2,1,DB_MODE_RO);
    ocrAddDependence(mirror_u64_guid(pv[P_BODY]),t2,2,DB_MODE_RO);
    ocrAddDependence(dir[pv[P_RANK]].db[0],t2,3,DB_MODE_RO);
    ocrAddDependence(av,t1,1,DB_MODE_NULL);
    ocrAddDependence(arrived,t1,0,DB_MODE_RO);
  } else {
    if(op==CP1) {
      ocrAddDependence(av,t2,1,DB_MODE_RO);
      for(int i=0;i<NDIR;++i)ocrAddDependence(bv,t2,2+i,DB_MODE_RO);
      ocrAddDependence(mirror_u64_guid(pv[P_BODY]),t2,8,DB_MODE_RO);
      ocrAddDependence(mirror_u64_guid(pv[P_DIR]),t2,9,DB_MODE_RO);for(u32 z=0;z<g->tree_count;++z)ocrAddDependence(g->tree[z].guid,t2,10+z,DB_MODE_RO);
      ocrAddDependence(bv,t1,1,DB_MODE_NULL);
      ocrAddDependence(mirror_u64_guid(pv[P_BODY]),t1,2,DB_MODE_RW);
      ocrAddDependence(mirror_u64_guid(pv[P_DIR]),t1,3,DB_MODE_RO);for(u32 z=0;z<g->tree_count;++z)ocrAddDependence(g->tree[z].guid,t1,4+z,DB_MODE_RO);
    } else if(op==CH1) {
      ocrAddDependence(av,t2,1,DB_MODE_RO);ocrAddDependence(bv,t2,2,DB_MODE_RO);
      ocrAddDependence(dir[cellid].db[0],t2,3,DB_MODE_RO);
      ocrAddDependence(bv,t1,1,DB_MODE_NULL);ocrAddDependence(dir[cellid].db[0],t1,2,DB_MODE_RO);
    } else if(op==NB1) {
      ocrAddDependence(av,t2,1,DB_MODE_RO);ocrAddDependence(bv,t2,2,DB_MODE_RO);
      ocrAddDependence(bv,t1,1,DB_MODE_NULL);
    }
    ocrAddDependence(av,t1,0,DB_MODE_RO);
  }
  ocrAddDependence(first,t2,0,op==CM1?DB_MODE_RO:DB_MODE_RW);
  return result;
}
static ocrGuid_t reap_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;
  pool_t *pool=d[dc-1].ptr;
  pool_lock(pool);
  if(pool->count==pool->capacity)abort();
  pool->free[pool->count++]=d[0].guid;
  atomic_flag_clear_explicit(&pool->mutex,memory_order_release);
  ocrEventDestroy(mirror_u64_guid(pv[P_FIRST]));return NULL_GUID;
}
static ocrGuid_t clean_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;
  cell_ref_t *dir=d[dc-2].ptr;
  pool_t *pool=d[dc-1].ptr;
  pool_lock(pool);
  while(pool->count)ocrDbDestroy(pool->free[--pool->count]);
  atomic_flag_clear_explicit(&pool->mutex,memory_order_release);
  ocrDbDestroy(d[dc-1].guid);
  for(u32 i=0;i<CELL_COUNT;++i)for(int k=0;k<=NLIST;++k)
    if(!ocrGuidIsNull(dir[i].db[k]))ocrDbDestroy(dir[i].db[k]);
  ocrDbDestroy(mirror_u64_guid(pv[P_BODY]));ocrDbDestroy(mirror_u64_guid(pv[P_DIR]));
  for(u64 t=0;t<pv[P_NT];++t)for(int i=0;i<NDIR;++i) {
    ocrEventDestroy(point(pv,t,i,pv[P_RANK],0));ocrEventDestroy(point(pv,t,i,pv[P_RANK],1));
  }
  ocrEventDestroy(mirror_u64_guid(pv[P_FIRST]));return NULL_GUID;
}
static ocrGuid_t graph_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;
  tree_view_t view={pv,d[0].ptr,d[1].ptr,d,2};cell_ref_t *dir=d[1].ptr;
  u64 local=pv[P_NP]/pv[P_NL],size=pv[P_SIZE];
  int ids[64];u64 count;
  for(u64 r=0;r<pv[P_NL];++r) {
    owned_cells(r,pv[P_NP],pv[P_NL],ids,&count);
    for(u64 j=0;j<count;++j)if(cell_at(&view,ids[j])->count[MEMBERS]>size) {
      PRINTF("nbody_hpx: partition size is below cell membership\n");ocrShutdown();return NULL_GUID;
    }
  }
  owned_cells(pv[P_RANK],pv[P_NP],pv[P_NL],ids,&count);
  if(pv[P_RANK]==0) {
    u64 *start;ocrGuid_t stamp=new_db(pv,sizeof(*start),(void**)&start);*start=mirror_now_ns();
    ocrDbRelease(stamp);ocrEventSatisfy(mirror_u64_guid(pv[P_START_EVENT]),stamp);
  }
  pool_t *pool=d[dc-1].ptr;
  graph_t g={.tree=d+2,.tree_count=(u32)pv[P_TREE_DEPS]};u32 u[2][64],parent[2];
  for(u64 j=0;j<local;++j) {
    body_t *out;ocrGuid_t db=pool_take(pv,pool,(void**)&out);
    if(!out)abort();
    const cell_t *c=cell_at(&view,ids[j]);
    const int *members=c->count[MEMBERS]?list_at(&view,ids[j],MEMBERS):NULL;
    for(u32 k=0;k<c->count[MEMBERS];++k) {
      int id=members[k];out[k]=view.b[id];out[k].ID1=id;
      view.b[id].force[0]=1;view.b[id].force[1]=1;view.b[id].force[2]=1;
      out[k].force[0]=1;out[k].force[1]=1;out[k].force[2]=1;
    }
    for(u64 k=c->count[MEMBERS];k<size;++k){out[k]=view.b[0];out[k].ID1=-1;}
    ocrDbRelease(db);ocrGuid_t e=new_event();u[0][j]=value_new(&g,e);ocrEventSatisfy(e,db);
  }
  body_t *empty;ocrGuid_t empty_db=pool_take(pv,pool,(void**)&empty);
  if(!empty)abort();
  for(u64 k=0;k<size;++k){empty[k]=view.b[0];empty[k].ID1=-1;}
  ocrDbRelease(empty_db);ocrGuid_t e=new_event();parent[0]=value_new(&g,e);ocrEventSatisfy(e,empty_db);
  notify_value(pv,&g,u[0][0],0);
  for(u64 t=0;t<pv[P_NT];++t) {
    u64 cur=t%2,nxt=(t+1)%2;pv[P_T]=t;
    for(int dirn=0;dirn<NDIR;++dirn) {
      pv[P_D]=(u64)dirn;ocrGuid_t incoming=point(pv,t,dirn,pv[P_RANK],0);mirror_edge_open(incoming);
      parent[nxt]=operation(pv,&g,dir,NM1,parent[cur],parent[cur],incoming,0);
    }
    for(u64 j=0;j<local;++j) {
      pv[P_J]=j;
      u[nxt][j]=operation(pv,&g,dir,CP1,u[cur][j],parent[nxt],NULL_GUID,ids[j]);
      u[cur][j]=operation(pv,&g,dir,CM1,u[nxt][j],u[nxt][j],NULL_GUID,ids[j]);
      for(int dirn=0;dirn<NDIR;++dirn) {
        pv[P_D]=(u64)dirn;
        u[nxt][j]=operation(pv,&g,dir,CH1,u[cur][j],parent[nxt],NULL_GUID,ids[j]);
      }
      for(int dirn=0;dirn<4;++dirn) {
        pv[P_D]=(u64)dirn;
        parent[nxt]=operation(pv,&g,dir,NB1,parent[cur],u[nxt][j],NULL_GUID,ids[j]);
      }
    }
    if(t+1<pv[P_NT])notify_value(pv,&g,parent[nxt],t+1);
  }
  ocrGuid_t report=mirror_u64_guid(pv[P_REPORT]);
  for(int gen=0;gen<2;++gen) {
    for(u64 j=0;j<local;++j)use_value(&g,u[gen][j],report);
    use_value(&g,parent[gen],report);
  }
  ocrGuid_t *result;ocrGuid_t result_db=new_db(pv,local*sizeof(*result),(void**)&result);
  for(u64 j=0;j<local;++j)result[j]=g.values[u[0][j]].value;
  ocrDbRelease(result_db);ocrGuid_t gathered=gather_point(pv,pv[P_RANK]);mirror_edge_open(gathered);
  ocrEventSatisfy(gathered,result_db);
  ocrGuid_t building=new_event();
  u64 params[P_COUNT];memcpy(params,pv,sizeof params);params[P_FIRST]=mirror_guid_u64(building);
  ocrHint_t h;mirror_rank_hint(&h,pv[P_RANK],OCR_HINT_EDT_T);
  ocrGuid_t cleanup,cleanup_out;
  ocrEdtCreate(&cleanup,mirror_u64_guid(pv[P_CLEAN_TPL]),P_COUNT,params,g.count+2,NULL,EDT_PROP_NONE,&h,&cleanup_out);
  ocrGuid_t closed=cleanup_point(pv,pv[P_RANK]);mirror_edge_open(closed);ocrAddDependence(cleanup_out,closed,0,DB_MODE_NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_DIR]),cleanup,g.count,DB_MODE_RO);
  ocrAddDependence(mirror_u64_guid(pv[P_POOL]),cleanup,g.count+1,DB_MODE_RW);
  for(u32 i=0;i<g.count;++i) {
    value_t *v=&g.values[i];params[P_FIRST]=mirror_guid_u64(v->value);
    ocrGuid_t reap,out;
    ocrEdtCreate(&reap,mirror_u64_guid(pv[P_REAP_TPL]),P_COUNT,params,v->count+3,NULL,EDT_PROP_NONE,&h,&out);
    ocrAddDependence(out,cleanup,i,DB_MODE_NULL);
    ocrAddDependence(v->value,reap,0,DB_MODE_NULL);
    for(u32 j=0;j<v->count;++j)ocrAddDependence(v->users[j],reap,j+2,DB_MODE_NULL);
    ocrAddDependence(mirror_u64_guid(pv[P_POOL]),reap,v->count+2,DB_MODE_RW);
    ocrAddDependence(building,reap,1,DB_MODE_NULL);free(v->users);
  }
  free(g.values);ocrEventSatisfy(building,NULL_GUID);return NULL_GUID;
}
static ocrGuid_t enum_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;
  cell_ref_t *dir=d[1].ptr;u32 count=0;
  for(u32 i=0;i<CELL_COUNT;++i)for(int k=0;k<=NLIST;++k)
    if(!ocrGuidIsNull(dir[i].db[k]))dir[i].slot[k]=count++;
  u64 q[P_COUNT];memcpy(q,pv,sizeof q);q[P_TREE_DEPS]=count;
  ocrGuid_t ready;
  ocrGuid_t task=make_task(q,pv[P_OP]?pv[P_GRAPH_TPL]:pv[P_LIST_TPL],count+2+(pv[P_OP]?1:0),pv[P_OP]?NULL:&ready);
  if(!pv[P_OP]) {
    q[P_OP]=1;q[P_FIRST]=mirror_guid_u64(ready);ocrGuid_t next=make_task(q,pv[P_ENUM_TPL],3,NULL);
    ocrAddDependence(mirror_u64_guid(pv[P_BODY]),next,0,DB_MODE_RW);
    ocrAddDependence(mirror_u64_guid(pv[P_DIR]),next,1,DB_MODE_RW);
    ocrAddDependence(ready,next,2,DB_MODE_NULL);
  }
  if(pv[P_OP])ocrAddDependence(mirror_u64_guid(pv[P_POOL]),task,count+2,DB_MODE_RW);
  wire_tree(task,2,dir,pv[P_OP]?DB_MODE_RO:DB_MODE_RW);
  ocrAddDependence(mirror_u64_guid(pv[P_BODY]),task,0,pv[P_OP]?DB_MODE_RW:DB_MODE_RO);
  ocrAddDependence(mirror_u64_guid(pv[P_DIR]),task,1,pv[P_OP]?DB_MODE_RO:DB_MODE_RW);
  if(pv[P_FIRST])ocrEventDestroy(mirror_u64_guid(pv[P_FIRST]));
  return NULL_GUID;
}
static ocrGuid_t driver_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;(void)d;
  body_t *b;cell_ref_t *dir;cell_t *root;int *members;
  u64 cap=pv[P_NP]/pv[P_NL]+1+pv[P_NT]*(6+12*(pv[P_NP]/pv[P_NL]));
  pool_t *pool;ocrGuid_t pool_db=new_db(pv,sizeof(*pool)+cap*sizeof(ocrGuid_t),(void**)&pool);
  atomic_flag_clear(&pool->mutex);pool->count=0;pool->capacity=cap;
  pv[P_POOL]=mirror_guid_u64(pool_db);ocrDbRelease(pool_db);
  ocrGuid_t body=new_db(pv,pv[P_N]*sizeof(*b),(void**)&b);
  ocrGuid_t directory=new_db(pv,CELL_COUNT*sizeof(*dir),(void**)&dir);memset(dir,0,CELL_COUNT*sizeof(*dir));
  pv[P_BODY]=mirror_guid_u64(body);pv[P_DIR]=mirror_guid_u64(directory);
  struct random_data random={0};int32_t state[32],value;
  initstate_r(1,(char*)state,sizeof state,&random);
  body_t initial={0};
  for(int j=0;j<3;++j){initial.r1[j]=1;initial.v1[j]=1;initial.force[j]=0;}
  for(u64 i=0;i<pv[P_N];++i)b[i]=initial;
  for(u64 i=0;i<pv[P_N];++i) {
    b[i].ID1=(int)i;b[i].parent=0;b[i].m1=1;
    for(int j=0;j<3;++j){random_r(&random,&value);b[i].r1[j]=(float)(value%100);b[i].v1[j]=0;}
  }
  ocrGuid_t root_db=dir[0].db[0]=new_db(pv,sizeof(*root),(void**)&root);memset(root,0,sizeof(*root));
  root->NumNodes=(int)pv[P_N];root->count[MEMBERS]=(u32)pv[P_N];
  for(int j=0;j<3;++j){root->boundary[2*j]=0;root->boundary[2*j+1]=100;}
  for(int j=0;j<6;++j)root->neighbors[j]=-1;
  ocrGuid_t member_db=dir[0].db[MEMBERS+1]=new_db(pv,pv[P_N]*sizeof(*members),(void**)&members);
  for(u64 i=0;i<pv[P_N];++i)members[i]=(int)i;
  for(u64 i=0;i<pv[P_N];++i)for(int j=0;j<3;++j) {
    if(b[i].r1[j]<root->boundary[2*j])root->boundary[2*j]=b[i].r1[j];
    if(b[i].r1[j]>root->boundary[2*j+1])root->boundary[2*j+1]=b[i].r1[j];
  }
  mass(root,members,b);if(!create_children(pv,root,dir))return NULL_GUID;
  ocrGuid_t children[8];for(int i=0;i<8;++i)children[i]=dir[i+1].db[0];
  ocrDbRelease(body);ocrDbRelease(directory);ocrDbRelease(root_db);ocrDbRelease(member_db);
  ocrHint_t h;mirror_rank_hint(&h,pv[P_RANK],OCR_HINT_EDT_T);
  ocrGuid_t tree,out,ready=new_event();
  ocrEdtCreate(&tree,mirror_u64_guid(pv[P_TREE_TPL]),P_COUNT,pv,12,NULL,EDT_PROP_FINISH,&h,&out);
  ocrAddDependence(out,ready,0,DB_MODE_NULL);
  pv[P_OP]=0;pv[P_FIRST]=mirror_guid_u64(ready);
  ocrGuid_t after=make_task(pv,pv[P_ENUM_TPL],3,NULL);
  ocrAddDependence(body,after,0,DB_MODE_RO);ocrAddDependence(directory,after,1,DB_MODE_RW);ocrAddDependence(ready,after,2,DB_MODE_NULL);
  ocrAddDependence(body,tree,0,DB_MODE_RW);ocrAddDependence(directory,tree,1,DB_MODE_RW);
  ocrAddDependence(root_db,tree,2,DB_MODE_RW);ocrAddDependence(member_db,tree,3,DB_MODE_RO);
  for(int i=0;i<8;++i)ocrAddDependence(children[i],tree,4+i,DB_MODE_RW);
  return NULL_GUID;
}
typedef struct {u64 start,index,cells,pass;double sum;ocrGuid_t values[];} result_t;
static void next_pull(u64 *pv,ocrGuid_t db,result_t *r) {
  ocrGuid_t value=r->values[r->index];ocrDbRelease(db);
  ocrGuid_t task=make_task(pv,pv[P_PULL_TPL],2,NULL);
  ocrAddDependence(value,task,1,DB_MODE_RO);ocrAddDependence(db,task,0,DB_MODE_RW);
}
static ocrGuid_t pull_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;result_t *r=d[0].ptr;
  if(r->pass) {
    const body_t *b=d[1].ptr;
    for(u64 i=0;i<pv[P_SIZE];++i){r->sum+=b[i].r1[0];r->sum+=b[i].r1[1];r->sum+=b[i].r1[2];}
  }
  if(++r->index==r->cells){r->index=0;++r->pass;}
  if(r->pass<2)next_pull(pv,d[0].guid,r);
  else {
    mirror_app_e2e(r->start);PRINTF("CHECKSUM %.14e\n",r->sum);
    ocrDbDestroy(d[0].guid);ocrDbRelease(d[1].guid);
    ocrEventSatisfy(mirror_u64_guid(pv[P_REPORT]),NULL_GUID);
  }
  return NULL_GUID;
}
static ocrGuid_t collect_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;u64 local=pv[P_NP]/pv[P_NL],cells=local*pv[P_NL];result_t *r;
  ocrGuid_t db=new_db(pv,sizeof(*r)+cells*sizeof(ocrGuid_t),(void**)&r);
  r->start=*(u64*)d[pv[P_NL]].ptr;r->index=r->pass=0;r->cells=cells;r->sum=0;
  for(u64 rank=0;rank<pv[P_NL];++rank) {
    memcpy(&r->values[rank*local],d[rank].ptr,local*sizeof(ocrGuid_t));
    ocrDbDestroy(d[rank].guid);ocrEventDestroy(gather_point(pv,rank));
  }
  ocrDbDestroy(d[pv[P_NL]].guid);ocrEventDestroy(mirror_u64_guid(pv[P_START_EVENT]));
  next_pull(pv,db,r);return NULL_GUID;
}
static ocrGuid_t close_edt(u32 pc,u64 *pv,u32 dc,ocrEdtDep_t d[]) {
  (void)pc;(void)dc;(void)d;
  for(u64 r=0;r<pv[P_NL];++r)ocrEventDestroy(cleanup_point(pv,r));
  ocrEventDestroy(mirror_u64_guid(pv[P_REPORT]));ocrShutdown();return NULL_GUID;
}
ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb);
  u64 n = 1000000, nt = 100, size = 150000, th = 100, kth = 500, np = 0;
  double theta = 0.3;
  int bad = mirror_option_u64(argdb, argc, "--n", &n) < 0
         || mirror_option_u64(argdb, argc, "--nt", &nt) < 0
         || mirror_option_u64(argdb, argc, "--size", &size) < 0
         || mirror_option_f64(argdb, argc, "--theta", &theta) < 0
         || mirror_option_u64(argdb, argc, "--th", &th) < 0
         || mirror_option_u64(argdb, argc, "--k-th", &kth) < 0
         || mirror_option_u64(argdb, argc, "--np", &np) < 0;

  u64 nl;
  ocrAffinityCount(AFFINITY_PD, &nl);
  /* The octree level the cell map is read at, by the rule the program states:
   * the shallow one below nine ranks and the deeper one from nine. */
  if (np == 0) np = nl < 9 ? 8 : 64;

  u64 points = 2 * NDIR;
  int sane = !bad && nl != 0 && n >= 1 && nt >= 1 && th >= 1 && kth >= 1
          && size >= kth && (np == 8 || np == 64) && nl <= np
          && n <= 0x7fffffffu && size <= 0xffffffffu;
  sane = sane && mirror_fits(&points, nt, 0xffffffffu - 2);
  points += 2;
  sane = sane && mirror_fits(&points, nl, 0xffffffffu);
  /* Every direction's neighbour map has to be a permutation of the ranks, and
   * that is the property the rendezvous rests on rather than mere range: two
   * ranks with one neighbour would put two producers on a point and leave a
   * third rank's point unsatisfied, and a neighbour outside the world is
   * nobody's.  The table the map reads is written for a few rank counts and
   * answers the others with a step that leaves it. */
  for (int d = 0; sane && d < NDIR; ++d)
    for (u64 r = 0; sane && r < nl; ++r) {
      u64 to = neighbour_index(r, DIR_STEP[d], nl);
      if (to >= nl) { sane = 0; break; }
      for (u64 q = 0; q < r; ++q)
        if (neighbour_index(q, DIR_STEP[d], nl) == to) { sane = 0; break; }
    }
  ocrGuid_t range = NULL_GUID;
  if (sane && ocrGuidRangeCreate(&range, points, GUID_USER_EVENT_STICKY) != 0)
    sane = 0;
  if (!sane) {
    PRINTF("nbody_hpx: usage --n=N --nt=T --size=S --theta=X --th=H --k-th=K"
           " [--np=P] (N >= 1, T >= 1, H >= 1, K >= 1, S >= K, P is 8 or 64 and"
           " at least the rank count, a rank count whose neighbour table is a"
           " permutation of the ranks in every direction, and a name space of"
           " (12*T+2)*ranks below 2^32)\n");
    ocrShutdown();
    return NULL_GUID;
  }

  ocrGuid_t tree, enumerate, lists, graph, stage, reap, clean, collect, pull, close, driver, allocate;
  ocrEdtTemplateCreate(&allocate,allocate_edt,P_COUNT,3);
  ocrEdtTemplateCreate(&tree,subtree_edt,P_COUNT,12);
  ocrEdtTemplateCreate(&enumerate,enum_edt,P_COUNT,3);
  ocrEdtTemplateCreate(&lists,list_edt,P_COUNT,EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&graph,graph_edt,P_COUNT,EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&stage,stage_edt,P_COUNT,EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&reap,reap_edt,P_COUNT,EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&clean,clean_edt,P_COUNT,EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&collect,collect_edt,P_COUNT,EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&pull,pull_edt,P_COUNT,2);
  ocrEdtTemplateCreate(&close,close_edt,P_COUNT,EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&driver,driver_edt,P_COUNT,0);
  u64 pv[P_COUNT]={0};pv[P_N]=n;pv[P_NT]=nt;pv[P_SIZE]=size;pv[P_TH]=th;pv[P_KTH]=kth;
  pv[P_NP]=np;pv[P_NL]=nl;memcpy(&pv[P_THETA],&theta,sizeof theta);
  pv[P_ALLOC_TPL]=mirror_guid_u64(allocate);
  pv[P_RANGE]=mirror_guid_u64(range);pv[P_TREE_TPL]=mirror_guid_u64(tree);
  pv[P_ENUM_TPL]=mirror_guid_u64(enumerate);pv[P_LIST_TPL]=mirror_guid_u64(lists);
  pv[P_GRAPH_TPL]=mirror_guid_u64(graph);pv[P_STAGE_TPL]=mirror_guid_u64(stage);
  pv[P_REAP_TPL]=mirror_guid_u64(reap);pv[P_CLEAN_TPL]=mirror_guid_u64(clean);
  pv[P_COLLECT_TPL]=mirror_guid_u64(collect);pv[P_PULL_TPL]=mirror_guid_u64(pull);
  ocrGuid_t report=new_event(),start=new_event();
  pv[P_REPORT]=mirror_guid_u64(report);pv[P_START_EVENT]=mirror_guid_u64(start);
  ocrGuid_t collector=make_task(pv,pv[P_COLLECT_TPL],(u32)nl+1,NULL);
  ocrGuid_t closer=make_task(pv,mirror_guid_u64(close),(u32)nl+1,NULL);
  for(u64 r=0;r<nl;++r) {
    ocrGuid_t a=gather_point(pv,r),b=cleanup_point(pv,r);mirror_edge_open(a);mirror_edge_open(b);
    ocrAddDependence(a,collector,(u32)r,DB_MODE_RO);ocrAddDependence(b,closer,(u32)r,DB_MODE_NULL);
  }
  ocrAddDependence(start,collector,(u32)nl,DB_MODE_RO);ocrAddDependence(report,closer,(u32)nl,DB_MODE_NULL);
  mirror_spmd_fork(driver,pv,P_COUNT,P_RANK,nl,0,NULL);return NULL_GUID;
}
