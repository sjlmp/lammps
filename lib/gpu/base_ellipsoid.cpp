/***************************************************************************
                              base_ellipsoid.cpp
                             -------------------
                               W. Michael Brown

  Base class for acceleration of ellipsoid potentials

 __________________________________________________________________________
    This file is part of the LAMMPS Accelerator Library (LAMMPS_AL)
 __________________________________________________________________________

    begin                : Thu May 5 2011
    email                : brownw@ornl.gov
 ***************************************************************************/

#include "base_ellipsoid.h"
using namespace LAMMPS_AL;

#ifdef USE_OPENCL
#include "ellipsoid_nbor_cl.h"
#else
#include "ellipsoid_nbor_ptx.h"
#endif

#define BaseEllipsoidT BaseEllipsoid<numtyp, acctyp>
extern PairGPUDevice<PRECISION,ACC_PRECISION> pair_gpu_device;

template <class numtyp, class acctyp>
BaseEllipsoidT::BaseEllipsoid() : _compiled(false), _max_bytes(0) {
  device=&pair_gpu_device;
  ans=new PairGPUAns<numtyp,acctyp>();
  nbor=new PairGPUNbor();
}

template <class numtyp, class acctyp>
BaseEllipsoidT::~BaseEllipsoid() {
  delete ans;
  delete nbor;
}

template <class numtyp, class acctyp>
int BaseEllipsoidT::bytes_per_atom(const int max_nbors) const {
  return device->atom.bytes_per_atom()+ans->bytes_per_atom()+
         nbor->bytes_per_atom(max_nbors);
}

template <class numtyp, class acctyp>
int BaseEllipsoidT::init_base(const int nlocal, const int nall,
                              const int max_nbors, const int maxspecial,
                              const double cell_size, const double gpu_split,
                              FILE *_screen, const int ntypes, int **h_form,
                              const char *ellipsoid_program,
                              const char *lj_program) {
  nbor_time_avail=false;
  screen=_screen;

  bool gpu_nbor=false;
  if (device->gpu_mode()==PairGPUDevice<numtyp,acctyp>::GPU_NEIGH)
    gpu_nbor=true;

  int _gpu_host=0;
  int host_nlocal=hd_balancer.first_host_count(nlocal,gpu_split,gpu_nbor);
  if (host_nlocal>0)
    _gpu_host=1;

  _threads_per_atom=device->threads_per_charge();
    
  int success=device->init(*ans,false,true,nlocal,host_nlocal,nall,nbor,
                           maxspecial,_gpu_host,max_nbors,cell_size,true);
  if (success!=0)
    return success;

  ucl_device=device->gpu;
  atom=&device->atom;

  _block_size=device->pair_block_size();
  compile_kernels(*ucl_device,ellipsoid_program,lj_program);

  // Initialize host-device load balancer
  hd_balancer.init(device,gpu_nbor,gpu_split);

  // Initialize timers for the selected GPU
  time_lj.init(*ucl_device);
  time_nbor1.init(*ucl_device);
  time_ellipsoid.init(*ucl_device);
  time_nbor2.init(*ucl_device);
  time_ellipsoid2.init(*ucl_device);
  time_lj.zero();
  time_nbor1.zero();
  time_ellipsoid.zero();
  time_nbor2.zero();
  time_ellipsoid2.zero();

  // See if we want fast GB-sphere or sphere-sphere calculations
  _host_form=h_form;
  _multiple_forms=false;
  for (int i=1; i<ntypes; i++)
    for (int j=i; j<ntypes; j++) 
      if (_host_form[i][j]!=ELLIPSE_ELLIPSE)
        _multiple_forms=true;
  if (_multiple_forms && host_nlocal>0) {
    std::cerr << "Cannot use Gayberne with multiple forms and GPU neighbor.\n";
    exit(1);
  }
  
  if (_multiple_forms)
    ans->dev_ans.zero();

  // Memory for ilist ordered by particle type
  if (host_olist.alloc(nbor->max_atoms(),*ucl_device)==UCL_SUCCESS)
    return 0;
  else return -3;

  _max_an_bytes=ans->gpu_bytes()+nbor->gpu_bytes();

  return 0;
}

template <class numtyp, class acctyp>
void BaseEllipsoidT::estimate_gpu_overhead() {
  device->estimate_gpu_overhead(2,_gpu_overhead,_driver_overhead);
}

template <class numtyp, class acctyp>
void BaseEllipsoidT::clear_base() {
  // Output any timing information
  output_times();
  host_olist.clear();
  
  if (_compiled) {
    k_nbor_fast.clear();
    k_nbor.clear();
    k_ellipsoid.clear();
    k_sphere_ellipsoid.clear();
    k_lj_fast.clear();
    k_lj.clear();
    delete nbor_program;
    delete ellipsoid_program;
    delete lj_program;
    _compiled=false;
  }
 
  time_nbor1.clear();
  time_ellipsoid.clear();
  time_nbor2.clear();
  time_ellipsoid2.clear();
  time_lj.clear();
  hd_balancer.clear();

  nbor->clear();
  ans->clear();
  device->clear();
}

template <class numtyp, class acctyp>
void BaseEllipsoidT::output_times() {
  // Output any timing information
  acc_timers();
  double single[9], times[9];

  single[0]=atom->transfer_time()+ans->transfer_time();
  single[1]=nbor->time_nbor.total_seconds();
  single[2]=time_nbor1.total_seconds()+time_nbor2.total_seconds()+
            nbor->time_nbor.total_seconds();
  single[3]=time_ellipsoid.total_seconds()+time_ellipsoid2.total_seconds();
  if (_multiple_forms)
    single[4]=time_lj.total_seconds();
  else
    single[4]=0;
  single[5]=atom->cast_time()+ans->cast_time();
  single[6]=_gpu_overhead;
  single[7]=_driver_overhead;
  single[8]=ans->cpu_idle_time();

  MPI_Reduce(single,times,9,MPI_DOUBLE,MPI_SUM,0,device->replica());
  double avg_split=hd_balancer.all_avg_split();

  _max_bytes+=atom->max_gpu_bytes();
  double mpi_max_bytes;
  MPI_Reduce(&_max_bytes,&mpi_max_bytes,1,MPI_DOUBLE,MPI_MAX,0,
             device->replica());
  double max_mb=mpi_max_bytes/(1024*1024);

  if (device->replica_me()==0)
    if (screen && times[5]>0.0) {
      int replica_size=device->replica_size();

      fprintf(screen,"\n\n-------------------------------------");
      fprintf(screen,"--------------------------------\n");
      fprintf(screen,"      GPU Time Info (average): ");
      fprintf(screen,"\n-------------------------------------");
      fprintf(screen,"--------------------------------\n");

      if (device->procs_per_gpu()==1) {
        fprintf(screen,"Data Transfer:   %.4f s.\n",times[0]/replica_size);
        fprintf(screen,"Data Cast/Pack:  %.4f s.\n",times[5]/replica_size);
        fprintf(screen,"Neighbor copy:   %.4f s.\n",times[1]/replica_size);
        if (nbor->gpu_nbor())
          fprintf(screen,"Neighbor build:  %.4f s.\n",times[2]/replica_size);
        else
          fprintf(screen,"Neighbor unpack: %.4f s.\n",times[2]/replica_size);
        fprintf(screen,"Force calc:      %.4f s.\n",times[3]/replica_size);
        fprintf(screen,"LJ calc:         %.4f s.\n",times[4]/replica_size);
      }
      fprintf(screen,"GPU Overhead:    %.4f s.\n",times[6]/replica_size);
      fprintf(screen,"Average split:   %.4f.\n",avg_split);
      fprintf(screen,"Max Mem / Proc:  %.2f MB.\n",max_mb);
      fprintf(screen,"CPU Driver_Time: %.4f s.\n",times[7]/replica_size);
      fprintf(screen,"CPU Idle_Time:   %.4f s.\n",times[8]/replica_size);
      fprintf(screen,"-------------------------------------");
      fprintf(screen,"--------------------------------\n\n");


      fprintf(screen,"Average split:   %.4f.\n",avg_split);
      fprintf(screen,"Max Mem / Proc:  %.2f MB.\n",max_mb);
    }
  _max_bytes=0.0;
}

// ---------------------------------------------------------------------------
// Pack neighbors to limit thread divergence for lj-lj and ellipse 
// ---------------------------------------------------------------------------
template<class numtyp, class acctyp>
void BaseEllipsoidT::pack_nbors(const int GX, const int BX, const int start, 
                                const int inum, const int form_low,
                                const int form_high, const bool shared_types,
                                int ntypes) {
  int stride=nbor->nbor_pitch();
  int anall=atom->nall();
  if (shared_types) {
    k_nbor_fast.set_size(GX,BX);
    k_nbor_fast.run(&atom->dev_x.begin(), &cut_form.begin(), 
                    &nbor->dev_nbor.begin(), &stride, &start, &inum,
                    &nbor->dev_packed.begin(), &form_low, &form_high, &anall);
  } else {
    k_nbor.set_size(GX,BX);
    k_nbor.run(&atom->dev_x.begin(), &cut_form.begin(), &ntypes,
               &nbor->dev_nbor.begin(), &stride, &start, &inum, 
               &nbor->dev_packed.begin(), &form_low, &form_high, &anall);
  }
}

// ---------------------------------------------------------------------------
// Copy neighbor list from host
// ---------------------------------------------------------------------------
template <class numtyp, class acctyp>
void BaseEllipsoidT::reset_nbors(const int nall, const int inum, 
                                 const int osize, int *ilist,
                                 int *numj, int *type, int **firstneigh,
                                 bool &success) {
  success=true;
    
  nbor_time_avail=true;

  int mn=nbor->max_nbor_loop(inum,numj,ilist);
  resize_atom(nall,success);
  resize_local(inum,0,mn,osize,success);
  if (!success)
    return;
    
  if (_multiple_forms) {
    int p=0;
    for (int i=0; i<osize; i++) {
      int itype=type[ilist[i]];
      if (_host_form[itype][itype]==ELLIPSE_ELLIPSE) {
        host_olist[p]=ilist[i];
        p++;
      }
    }
    _max_last_ellipse=p;
    _last_ellipse=std::min(inum,_max_last_ellipse);
    for (int i=0; i<osize; i++) {
      int itype=type[ilist[i]];
      if (_host_form[itype][itype]!=ELLIPSE_ELLIPSE) {
        host_olist[p]=ilist[i];
        p++;
      }
    }
    nbor->get_host(inum,host_olist.begin(),numj,firstneigh,block_size());
    nbor->copy_unpacked(inum,mn);
    return;
  }
  _last_ellipse=inum;
  _max_last_ellipse=inum;
  nbor->get_host(inum,ilist,numj,firstneigh,block_size());
  nbor->copy_unpacked(inum,mn);

  double bytes=ans->gpu_bytes()+nbor->gpu_bytes();
  if (bytes>_max_an_bytes)
    _max_an_bytes=bytes;
}

// ---------------------------------------------------------------------------
// Build neighbor list on device
// ---------------------------------------------------------------------------
template <class numtyp, class acctyp>
inline void BaseEllipsoidT::build_nbor_list(const int inum, const int host_inum,
                                            const int nall, double **host_x,
                                            int *host_type, double *sublo,
                                            double *subhi, int *tag, 
                                            int **nspecial, int **special,
                                            bool &success) {
  nbor_time_avail=true;

  success=true;
  resize_atom(nall,success);
  resize_local(inum,host_inum,nbor->max_nbors(),0,success);
  if (!success)
    return;
  atom->cast_copy_x(host_x,host_type);

  int mn;
  nbor->build_nbor_list(inum, host_inum, nall, *atom, sublo, subhi, tag,
                        nspecial, special, success, mn);
  nbor->copy_unpacked(inum,mn);
  _last_ellipse=inum;
  _max_last_ellipse=inum;

  double bytes=ans->gpu_bytes()+nbor->gpu_bytes();
  if (bytes>_max_an_bytes)
    _max_an_bytes=bytes;
}

// ---------------------------------------------------------------------------
// Copy nbor list from host if necessary and then calculate forces, virials,..
// ---------------------------------------------------------------------------
template <class numtyp, class acctyp>
int* BaseEllipsoidT::compute(const int f_ago, const int inum_full,
                             const int nall, double **host_x, int *host_type,
                             int *ilist, int *numj, int **firstneigh,
                             const bool eflag, const bool vflag,
                             const bool eatom, const bool vatom,
                             int &host_start, const double cpu_time,
                             bool &success, double **host_quat) {
  acc_timers();
  if (inum_full==0) {
    host_start=0;
    zero_timers();
    return NULL;
  }
  
  int ago=hd_balancer.ago_first(f_ago);
  int inum=hd_balancer.balance(ago,inum_full,cpu_time);
  ans->inum(inum);
  _last_ellipse=std::min(inum,_max_last_ellipse);
  host_start=inum;

  if (ago==0) {
    reset_nbors(nall, inum, inum_full, ilist, numj, host_type, firstneigh,
                success);
    if (!success)
      return NULL;
  }
  int *list;
  if (_multiple_forms)
    list=host_olist.begin();
  else
    list=ilist;

  atom->cast_x_data(host_x,host_type);
  atom->cast_quat_data(host_quat[0]);
  hd_balancer.start_timer();
  atom->add_x_data(host_x,host_type);
  atom->add_quat_data();

  loop(eflag,vflag);
  ans->copy_answers(eflag,vflag,eatom,vatom,list);
  device->add_ans_object(ans);
  hd_balancer.stop_timer();
  return list;
}

// ---------------------------------------------------------------------------
// Reneighbor on GPU if necessary and then compute forces, virials, energies
// ---------------------------------------------------------------------------
template <class numtyp, class acctyp>
int** BaseEllipsoidT::compute(const int ago, const int inum_full, const int nall,
                              double **host_x, int *host_type, double *sublo,
                              double *subhi, int *tag, int **nspecial,
                              int **special, const bool eflag, const bool vflag,
                              const bool eatom, const bool vatom, 
                              int &host_start, int **ilist, int **jnum,
                              const double cpu_time, bool &success,
                              double **host_quat) {
  acc_timers();
  if (inum_full==0) {
    host_start=0;
    zero_timers();
    return NULL;
  }

  hd_balancer.balance(cpu_time);
  int inum=hd_balancer.get_gpu_count(ago,inum_full);
  ans->inum(inum);
  _last_ellipse=std::min(inum,_max_last_ellipse);
  host_start=inum;
  
  // Build neighbor list on GPU if necessary
  if (ago==0) {
    build_nbor_list(inum, inum_full-inum, nall, host_x, host_type,
                    sublo, subhi, tag, nspecial, special, success);
    if (!success)
      return NULL;
    atom->cast_quat_data(host_quat[0]);
    hd_balancer.start_timer();
  } else {    
    atom->cast_x_data(host_x,host_type);
    atom->cast_quat_data(host_quat[0]);
    hd_balancer.start_timer();
    atom->add_x_data(host_x,host_type);
  }

  atom->add_quat_data();
  *ilist=nbor->host_ilist.begin();
  *jnum=nbor->host_acc.begin();

  loop(eflag,vflag);
  ans->copy_answers(eflag,vflag,eatom,vatom);
  device->add_ans_object(ans);
  hd_balancer.stop_timer();
  return nbor->host_jlist.begin()-host_start;
}

template <class numtyp, class acctyp>
double BaseEllipsoidT::host_memory_usage_base() const {
  return device->atom.host_memory_usage()+nbor->host_memory_usage()+
         4*sizeof(numtyp)+sizeof(BaseEllipsoid<numtyp,acctyp>);
}

template <class numtyp, class acctyp>
void BaseEllipsoidT::compile_kernels(UCL_Device &dev, 
                                     const char *ellipsoid_string,
                                     const char *lj_string) {
  if (_compiled)
    return;

  std::string flags="-cl-fast-relaxed-math -cl-mad-enable "+
                    std::string(OCL_PRECISION_COMPILE);

  nbor_program=new UCL_Program(dev);
  nbor_program->load_string(ellipsoid_nbor,flags.c_str());
  k_nbor_fast.set_function(*nbor_program,"kernel_nbor_fast");
  k_nbor.set_function(*nbor_program,"kernel_nbor");

  ellipsoid_program=new UCL_Program(dev);
  ellipsoid_program->load_string(ellipsoid_string,flags.c_str());
  k_ellipsoid.set_function(*ellipsoid_program,"kernel_ellipsoid");

  lj_program=new UCL_Program(dev);
  lj_program->load_string(lj_string,flags.c_str());
  k_sphere_ellipsoid.set_function(*lj_program,"kernel_sphere_ellipsoid");
  k_lj_fast.set_function(*lj_program,"kernel_lj_fast");
  k_lj.set_function(*lj_program,"kernel_lj");

  _compiled=true;
}

template class BaseEllipsoid<PRECISION,ACC_PRECISION>;

