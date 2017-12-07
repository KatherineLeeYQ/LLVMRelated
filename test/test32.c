#include <stdlib.h>
struct fptr
{
int (*p_fptr)(int, int);
};
struct fsptr
{
struct fptr * sptr;
};
struct wfsptr
{
  struct fsptr * wfptr;
};
int plus(int a, int b) {
   return a+b;
}

int minus(int a, int b) {
   return a-b;
}

struct fptr * foo(int a, int b, struct wfsptr * i_fptr, struct wfsptr * j_fptr) {
   if(a>0 && b<0)
   {
    struct fsptr * temp=i_fptr->wfptr;
    i_fptr->wfptr->sptr = j_fptr->wfptr->sptr;
    j_fptr->wfptr->sptr =temp->sptr;
    return i_fptr->wfptr->sptr;
   }
   return j_fptr->wfptr->sptr;
} 

struct fptr * clever(int a, int b, struct fsptr * x_fptr, struct fsptr * y_fptr ) {
   struct wfsptr t1_fptr;
   t1_fptr.wfptr=x_fptr;
   struct wfsptr t2_fptr;
   t2_fptr.wfptr=y_fptr;
   return foo(a,b,&t1_fptr,&t2_fptr);
}


int moo(char x, int op1, int op2) {
    struct fptr a_fptr ;
    a_fptr.p_fptr=plus;
    struct fptr s_fptr ;
    s_fptr.p_fptr=minus;

    struct fsptr m_fptr;
    m_fptr.sptr=&a_fptr;
    struct fsptr n_fptr;
    n_fptr.sptr=&s_fptr;
    
    struct fptr* (*goo_ptr)(int, int, struct fsptr *,struct fsptr *);
    struct fptr* t_fptr = 0;

    t_fptr = clever(op1, op2, &m_fptr, &n_fptr);
    t_fptr->p_fptr(op1, op2);
    n_fptr.sptr->p_fptr(op1,op2);
    
    return 0;
}

// 38 : foo
// 56 : clever
// 57 : minus
// 58 : plus, minus