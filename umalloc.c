#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"

// Memory allocator by Kernighan and Ritchie,
// The C programming Language, 2nd ed.  Section 8.7.

typedef long Align;

union header {
  struct {
    union header *ptr;
    uint size;
  } s;
  Align x;
};
typedef struct link_header{
  struct link_header * next;
  
}link_list_header;

typedef union header Header;
static link_list_header *head;
static Header base;
static Header *freep;


void
free(void *ap)
{
  Header *bp, *p;
  if(is_pmalloc(ap))
    return;
  bp = (Header*)ap - 1;
  for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if(p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  if(bp + bp->s.size == p->s.ptr){
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else
    bp->s.ptr = p->s.ptr;
  if(p + p->s.size == bp){
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else
    p->s.ptr = bp;
  freep = p;
}

static Header*
morecore(uint nu, int isPmalloc)
{
  
  char *p;
  Header *hp;

  if(nu < 4096 && !isPmalloc)
    nu = 4096;
  p = sbrk(nu * sizeof(Header));
  if(p == (char*)-1)
    return 0;
  hp = (Header*)p;
  hp->s.size = nu;
  free((void*)(hp + 1));
  return freep;
}

void*
malloc(uint nbytes)
{
  Header *p, *prevp;
  uint nunits;

  nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    if(p->s.size >= nunits){
      if(p->s.size == nunits)
        prevp->s.ptr = p->s.ptr;
      else {
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      return (void*)(p + 1);
    }
    if(p == freep)
      if((p = morecore(nunits,0)) == 0)
        return 0;
  }
}


void*
pmalloc()
{
  void * ans;
  if(head){
    ans = (void*)head;
    head = head->next;
  }
  else{
    ans = sbrk(4096);
  }
  Update_Pmalloc(ans);
  return ans;
}

int 
protect_page(void* ap)
{
  return protected_page(ap);
}

int
pfree(void *ap)
{
  if(!is_protected(ap))
    return -1;
  link_list_header * new_head;
  update_unprotected(ap);
  pmalloc_Off(ap);
  new_head = (link_list_header*)ap;
  if(head){
    new_head->next = head;
    
  }else{
    
    new_head->next = 0;
    
  }
  head = new_head;
  return 1;
}
