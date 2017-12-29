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
    i_fptr->wfptr->sptr->p_fptr(a,b);
    return i_fptr->wfptr->sptr;
   }
   return j_fptr->wfptr->sptr;
} 

struct fptr * clever(int a, int b, struct fsptr * c_fptr, struct fsptr * v_fptr ) {
   struct wfsptr t1_fptr;
   t1_fptr.wfptr=c_fptr;
   struct wfsptr t2_fptr;
   t2_fptr.wfptr=v_fptr;
   return foo(a,b,&t1_fptr,&t2_fptr);
}

void make_simple_alias(struct wfsptr * q_fptr,struct fsptr * p_fptr)
{
  q_fptr->wfptr=p_fptr;
}
void make_alias(struct wfsptr* j_fptr,struct wfsptr * g_fptr)
{
  j_fptr->wfptr->sptr=g_fptr->wfptr->sptr;
}
void swap_w(struct wfsptr * w_fptr,struct wfsptr * e_fptr)
{
     struct wfsptr wftemp=*w_fptr;
     *w_fptr=*e_fptr;
     *e_fptr=wftemp;
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
    
    struct wfsptr w_fptr;
    w_fptr.wfptr=&m_fptr;

    struct wfsptr x_fptr;
    make_simple_alias(&x_fptr,&n_fptr);

    struct fsptr k_fptr;
    struct wfsptr y_fptr;
    y_fptr.wfptr= & k_fptr;
    make_alias(&y_fptr,&x_fptr);

    n_fptr.sptr=&s_fptr;

    struct fptr* t_fptr = 0;

    t_fptr = clever(op1, op2, &m_fptr, y_fptr.wfptr);
    t_fptr->p_fptr(op1, op2);
    m_fptr.sptr->p_fptr(op1,op2);
    swap_w(&x_fptr,&w_fptr);
    w_fptr.wfptr->sptr->p_fptr(op1,op2);
    
    return 0;
}

// 27 : minus
// 38 : foo
// 67 : make_simple_alias 
// 70 : make_simple_alias
// 75 : make_alias
// 81 : clever
// 82 : minus
// 83 : plus, minus
// 84 : swap_w
// 85 : minus