#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mount.h>
#include <asm/uaccess.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/version.h>

#include <linux/sched.h>

#include <asm/amd_nb.h>

#include "../definition_driver/x86_adapt_defs.h"

extern u32 x86_adapt_get_all_knobs_length(void);
extern struct knob_entry_definition * x86_adapt_get_all_knobs(void);

#define MODULE_NAME "x86_adapt"

/* allocates dynamicly a char device region, intializes it and adds it */
#define ALLOC_AND_INIT(DEV, FUNC, TYPE) \
    do { \
        err = alloc_chrdev_region(&(DEV##_device),0,num_possible_##TYPE##s()+1,#DEV); \
        if (err) { \
            printk(KERN_ERR "Failed to allocate chrdev_region for %s\n", #DEV); \
            goto fail; \
        } else { \
            DEV##_device_ok = 1 ; \
        } \
        FUNC(DEV, TYPE); \
        init_rwsem(&(DEV##_cdev.rwsem));\
        cdev_init(&(DEV##_cdev.cdev), & DEV##_fops); \
        DEV##_cdev.cdev.owner = THIS_MODULE; \
        err = cdev_add(&(DEV##_cdev.cdev), DEV##_device, num_possible_##TYPE##s()+1); \
        if (err) { \
            printk(KERN_ERR "Failed to add cdev for %s\n", #DEV); \
            goto fail; \
        } else { \
            DEV##_cdev_ok = 1 ; \
        } \
    } while (0)

/* allocates memory for a northbridge pci device and adds it */
#define ALLOC_UNCORE_PCI(NAME, NUM, NUM2) \
    do { \
        NAME = kmalloc(num_possible_nodes()*sizeof(struct pci_dev *),GFP_KERNEL); \
        if (NAME == NULL) { \
            err = -ENOMEM; \
            printk(KERN_ERR "Failed to allocate memory for %d %d\n", NUM,NUM2); \
            goto fail; \
        } \
        for_each_online_node(i) \
            NAME [i] = pci_get_bus_and_slot(get_uncore_bus_id(i), PCI_DEVFN(NUM,NUM2)); \
    } while (0)



/* allocates memory for a northbridge pci device and adds it */
#define ALLOC_NB_PCI(NUM) \
    do { \
        nb_f##NUM = kmalloc(num_possible_nodes()*sizeof(struct pci_dev *),GFP_KERNEL); \
        if (nb_f##NUM == NULL) { \
            err = -ENOMEM; \
            printk(KERN_ERR "Failed to allocate memory for nb_fd%d\n", NUM); \
            goto fail; \
        } \
        for_each_online_node(i) \
            if (node_to_amd_nb(i)!=NULL) \
              nb_f##NUM [i] = pci_get_bus_and_slot(0, PCI_DEVFN(PCI_SLOT(node_to_amd_nb(i)->misc->devfn),NUM)); \
            else \
              nb_f##NUM [i] = NULL; \
    } while (0)

/* checks if the char device was properly intialized and then it will be unregistered and deleted */
#define UNREGISTER_AND_DELETE(DEV, FUNC, TYPE) \
    do { \
        if (DEV##_cdev_ok) \
            cdev_del(&(DEV##_cdev.cdev)); \
        FUNC(DEV, TYPE); \
        if (DEV##_device_ok) \
            unregister_chrdev_region(DEV##_device, num_possible_##TYPE##s()+1); \
    } while (0)

/* frees the allocated space for this pci device */
#define FREE_PCI(NAME) \
    do { \
        if (NAME) { \
            for_each_node(i) { \
                if (NAME [i]) \
                    pci_dev_put (NAME [i]); \
            } \
            kfree(NAME); \
            NAME = NULL; \
        } \
    } while (0)

/* frees the allocated space for this pci device */
#define FREE_NB(NUM) \
    do { \
        if (nb_f##NUM) { \
            for_each_node(i) { \
                if (nb_f##NUM [i]) \
                    pci_dev_put (nb_f##NUM [i]); \
            } \
            kfree(nb_f##NUM); \
            nb_f##NUM = NULL; \
        } \
    } while (0)

/* adds all online devices */
#define DEVICE_CREATE(DEV, TYPE) \
    do { \
        struct device *dev; \
        for_each_online_##TYPE(i) { \
            dev = device_create(x86_adapt_class,NULL,MKDEV(MAJOR(DEV##_device), \
                MINOR(DEV##_device)+i),NULL,"%s_%d",#TYPE,i); \
            if (IS_ERR(dev)) { \
                    printk(KERN_ERR "Failed to create device /sys/class/x86_adapt%s%d\n",#TYPE,i); \
                    err = PTR_ERR(dev); \
                    goto fail; \
            } \
        } \
        dev = device_create(x86_adapt_class,NULL,MKDEV(MAJOR(DEV##_device), \
            MINOR(DEV##_device)+num_possible_##TYPE##s()),NULL,"%s_all",#TYPE); \
        if (IS_ERR(dev)) { \
            printk(KERN_ERR "Failed to create device /sys/class/x86_adapt/%s_all\n",#TYPE); \
            err = PTR_ERR(dev); \
            goto fail; \
        } \
    } while (0)

/* destroys all online devices */
#define DEVICE_DESTROY(DEV, TYPE) \
    do { \
        for_each_online_##TYPE(i) { \
            device_destroy(x86_adapt_class,MKDEV(MAJOR(DEV##_device), \
                MINOR(DEV##_device)+i)); \
        } \
            device_destroy(x86_adapt_class,MKDEV(MAJOR(DEV##_device), \
                MINOR(DEV##_device)+num_possible_##TYPE##s())); \
    } while (0)

/* create definition devices */
#define DEF_CREATE(DEV, TYPE) \
    do { \
        struct device *dev; \
        dev = device_create(x86_adapt_class,NULL,DEV##_device,NULL, \
            "%s_definitions",#TYPE);  \
        if (IS_ERR(dev)) { \
            printk(KERN_ERR "Failed to create device /sys/class/x86_adapt/%s_definitions\n",#TYPE); \
            err = PTR_ERR(dev); \
            goto fail; \
        } \
    } while (0)
                
/* destroy definition devices */
#define DEF_DESTROY(DEV, TYPE) \
    device_destroy(x86_adapt_class,DEV##_device)
    
#define X86_ADAPT_CPU 0
#define X86_ADAPT_NODE 1


struct x86_adapt_cdev {
    struct cdev cdev;
    struct rw_semaphore rwsem;
};

/* character devices */
static dev_t x86_adapt_cpu_device;
static dev_t x86_adapt_node_device;
static dev_t x86_adapt_def_cpu_device;
static dev_t x86_adapt_def_node_device;
static struct x86_adapt_cdev x86_adapt_cpu_cdev;
static struct x86_adapt_cdev x86_adapt_node_cdev;
static struct x86_adapt_cdev x86_adapt_def_cpu_cdev;
static struct x86_adapt_cdev x86_adapt_def_node_cdev;
/* initialize check variables */
static int x86_adapt_cpu_device_ok = 0;
static int x86_adapt_node_device_ok = 0;
static int x86_adapt_def_cpu_device_ok = 0;
static int x86_adapt_def_node_device_ok = 0;
static int x86_adapt_cpu_cdev_ok = 0;
static int x86_adapt_node_cdev_ok = 0;
static int x86_adapt_def_cpu_cdev_ok = 0;
static int x86_adapt_def_node_cdev_ok = 0;


/* temporary */
static struct class * x86_adapt_class;


/* TODO: refactor, so that the AMD devices dont use memory on Intel and vice versa */

/* AMD */
struct pci_dev ** nb_f0 = NULL;
struct pci_dev ** nb_f1 = NULL;
struct pci_dev ** nb_f2 = NULL;
struct pci_dev ** nb_f3 = NULL;
struct pci_dev ** nb_f4 = NULL;
struct pci_dev ** nb_f5 = NULL;

/* Intel Sandy Bridge EP */
struct pci_dev ** sb_pcu0 = NULL;
struct pci_dev ** sb_pcu1 = NULL;
struct pci_dev ** sb_pcu2 = NULL;

/* Intel Haswell EP */
struct pci_dev ** hsw_pcu0 = NULL;
struct pci_dev ** hsw_pcu1 = NULL;
struct pci_dev ** hsw_pcu2 = NULL;

/* artificial knob for resetting */
static struct knob_entry reset_knob =
{
    .name="RESET",
    .description="Writing ANY 8 bytes to this item will reset all settings for the accessed device (cpu/node)",
    .id=0,
    .length=1,
    .bitmask=1
};
#define KNOB_RESET 0

static u32 active_knobs_node_length = 0;
static struct knob_entry * active_knobs_node = NULL;

static u64 ** defaults_node = NULL;

static u32 active_knobs_cpu_length = 0;
static struct knob_entry * active_knobs_cpu = NULL;

static u64 ** defaults_cpu = NULL;

static u64 read_setting(int dev_nr, struct knob_entry knob) ;

/* This function gives you the bus id for uncore components of a NUMA node in sandy bridge ep processors */
static int get_uncore_bus_id(int node_id)
{
  if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL){
    struct pci_dev *ubox_dev = NULL;
    u32 config = 0;
    int devid = 0;

    /* dev id based on processor */

    /* based on arch/x86/kernel/cpu/perf_event_intel_uncore.c for Sandy Bridge / IvyTown */
    /* based on Datasheet 2 for Haswell EP */
    /* based on arch/x86/kernel/cpu/perf_event_intel_uncore_snbep.c for Broadwell EP */
    /* Sandy Bridge   : 0x3ce0 */
    /* Ivy Town:        0x0e1e*/
    /* Haswell EP:        0x2f1e*/
    /* Broadwell EP:        0x6f1e*/
    switch (boot_cpu_data.x86_model) {
      /* Sandy Bridge EP: 0x3ce0 */
      case 45: devid = 0x3ce0; break;
      case 62: devid = 0x0e1e; break;
      case 63: devid = 0x2f1e; break;
      case 86: devid = 0x6f1e; break;
      default:
        // printk(KERN_ERR "Unsupported Intel processor for Uncore stuff.\n");
        return -1;
        /* keep it at 0 */
    }

    while (1) {
      /* find the UBOX device */
      int bus;
      int nodeid;
      int err;
      ubox_dev = pci_get_device(PCI_VENDOR_ID_INTEL, devid, ubox_dev);
      if (!ubox_dev){
        printk(KERN_ERR MODULE_NAME"Could not get Intel Uncore device - missing ubox.\n");
        return -1;
      }
      bus = ubox_dev->bus->number;
      /* get the Node ID of the local register */
      err = pci_read_config_dword(ubox_dev, 0x40, &config);
      if (err){
        printk(KERN_ERR MODULE_NAME"Could not get Intel node id via pci %d.\n", err);
        return -1;
      }
      nodeid = config;
      if (nodeid == node_id){
        pci_dev_put(ubox_dev);
        /* printk (KERN_INFO "Given Node: %d, Found Node: %d, Bus: %d\n",node_id,nodeid, bus); */
        return bus;
      }
    }
    if (ubox_dev)
      pci_dev_put(ubox_dev);
    printk(KERN_ERR MODULE_NAME"This should not occur.\n");
    /* Should not occur */
    return -1;
  }
  /* Unsupported processor */
  return -1;

}

static void increment_knob_counter(u32 i, u32 *nr_knobs_cpu, u32 *nr_knobs_node) 
{
    struct knob_entry_definition * all_knobs = x86_adapt_get_all_knobs();
    if (all_knobs[i].knob.device == MSR) 
        (*nr_knobs_cpu)++; 
    else 
        (*nr_knobs_node)++; 
    return;
}

static void add_knob(u32 i, u32 *nr_knobs_cpu, u32 *nr_knobs_node) 
{
    struct knob_entry_definition * all_knobs = x86_adapt_get_all_knobs();
    if (all_knobs[i].knob.device == MSR) { 
        memcpy(&active_knobs_cpu[*nr_knobs_cpu],&(all_knobs[i].knob),
            sizeof (struct knob_entry)); 
        //printk(KERN_INFO "Added cpu feature %s\n", active_knobs_cpu[*nr_knobs_cpu].name);
        (*nr_knobs_cpu)++; 
    } else { 
        memcpy(&active_knobs_node[*nr_knobs_node],&(all_knobs[i].knob),
            sizeof (struct knob_entry)); 
        //printk(KERN_INFO "Added node feature %s\n", active_knobs_node[*nr_knobs_node].name);
        (*nr_knobs_node)++; 
    } 
    return;
}

/* traverses all knobs and either counts the avaible knobs or adds the avaible knobs */
static void traverse_knobs(u32 *nr_knobs_cpu, u32 *nr_knobs_node,
    void (*do_knob)(u32, u32*, u32*)) 
{
    u32 i,j,k,l,has_features;
    struct cpuinfo_x86 info = cpu_data(0);
    u8 vendor = info.x86_vendor;
    u8 family = info.x86;
    u8 model = info.x86_model;
    struct knob_entry_definition * all_knobs = x86_adapt_get_all_knobs();
    u32 all_knobs_length = x86_adapt_get_all_knobs_length();

    /* for all knobs do something */
    for (i = 0;i<all_knobs_length;i++) {
        /* if cpuid check failed, dont use this item */
        if (all_knobs[i].blocked_by_cpuid)
            continue;
      //printk("Check :%s\n",all_knobs[i].knob.name);
        for (j = 0;j<all_knobs[i].av_length;j++) {
            /* check vendor */
            if (vendor != all_knobs[i].av_vendors[j]->vendor)
                continue;
            /* check whether it has all the features wanted */
            has_features = 1;
            for (k = 0;k<all_knobs[i].av_vendors[j]->features_length;k++)
                if (!boot_cpu_has(all_knobs[i].av_vendors[j]->features[k]))
                    has_features = 0;
            if (!has_features)
                continue;
            if (all_knobs[i].av_vendors[j]->fam_length == 0) 
                do_knob(i, nr_knobs_cpu, nr_knobs_node);

            for (k = 0;k<all_knobs[i].av_vendors[j]->fam_length;k++) {
                /* check family */
                if (family != all_knobs[i].av_vendors[j]->fams[k].family)
                    continue;
                if (all_knobs[i].av_vendors[j]->fams[k].model_length == 0)
                    do_knob(i, nr_knobs_cpu, nr_knobs_node);
                else 
                    for (l = 0;l<all_knobs[i].av_vendors[j]->fams[k].model_length;l++)
                        /* check model */
                        if (model == all_knobs[i].av_vendors[j]->fams[k].models[l]) 
                            do_knob(i, nr_knobs_cpu, nr_knobs_node);
            }
        }
    }
    return;
}
static inline void free_defaults(void)
{
    int index;
    if (active_knobs_cpu != NULL)
        kfree(active_knobs_cpu);
    active_knobs_cpu = NULL;
    if (active_knobs_node != NULL)
        kfree(active_knobs_node);
    active_knobs_node = NULL;
    if (defaults_node != NULL)
    {
        for (index=0;index<num_possible_nodes();index++)
        {
            if (defaults_node[index]!=NULL)
                kfree(defaults_node[index]);
        }
        kfree(defaults_node);
        defaults_node = NULL;
    }
    if (defaults_cpu != NULL)
    {
        for (index=0;index<num_possible_cpus();index++)
        {
            if (defaults_cpu[index]!=NULL)
                kfree(defaults_cpu[index]);
        }
        kfree(defaults_cpu);
        defaults_cpu = NULL;
    }
}

static inline void read_defaults(void)
{
    u32 node, cpu, knob;
    /* read defaults */
    for (node=0;node<num_possible_nodes();node++)
    {
        for (knob=1;knob<active_knobs_node_length;knob++)
        {
             defaults_node[node][knob-1]=read_setting(node,active_knobs_node[knob]);
        }
    }
    
    for (cpu=0;cpu<num_possible_cpus();cpu++)
    {
        for (knob=1;knob<active_knobs_cpu_length;knob++)
        {
             defaults_cpu[cpu][knob-1]=read_setting(cpu,active_knobs_cpu[knob]);
        }
    }
}

/* used at init */
static int buildup_entries(void) 
{
    u32 nr_knobs_cpu = 0,nr_knobs_node = 0, index;

    /* count number of knobs */
    traverse_knobs(&nr_knobs_cpu, &nr_knobs_node, increment_knob_counter);

                    // printk("Found %d cpu features\n", nr_knobs_cpu);
                    // printk("Found %d node features\n", nr_knobs_node);

    /* allocate memory for the avaible knobs */


    active_knobs_cpu = kmalloc((nr_knobs_cpu+1)*sizeof(struct knob_entry),GFP_KERNEL);
    if (unlikely(active_knobs_cpu == NULL))
        return -ENOMEM;
    active_knobs_cpu_length = nr_knobs_cpu+1;

    active_knobs_node = kmalloc((nr_knobs_node+1)*sizeof(struct knob_entry),GFP_KERNEL);
    if (unlikely(active_knobs_node == NULL))
        goto fail;
    active_knobs_node_length = nr_knobs_node+1;

    /* allocate memory for defaults (node) */


    defaults_node = kzalloc(num_possible_nodes()*sizeof(u64*),GFP_KERNEL);
    if (unlikely(defaults_node == NULL))
        goto fail;
    for (index=0;index<num_possible_nodes();index++)
    {
        defaults_node[index] = kmalloc(nr_knobs_node*sizeof(u64),GFP_KERNEL);
        if (unlikely(defaults_node[index] == NULL))
             goto fail;
    }

    /* allocate memory for defaults (CPU) */


    defaults_cpu = kzalloc(num_possible_cpus()*sizeof(u64*),GFP_KERNEL);
    if (unlikely(defaults_cpu == NULL))
        goto fail;
    for (index=0;index<num_possible_cpus();index++)
    {
        defaults_cpu[index] = kmalloc(nr_knobs_cpu*sizeof(u64),GFP_KERNEL);
        if (unlikely(defaults_cpu[index] == NULL))
             goto fail;
    }



    /* add reset knobs */
    memcpy(&active_knobs_cpu[0],&reset_knob,sizeof(struct knob_entry));
    memcpy(&active_knobs_node[0],&reset_knob,sizeof(struct knob_entry));


    /* add knobs */
    nr_knobs_cpu = 1; /* already a reset knob, so there's already 1 */
    nr_knobs_node = 1;

    traverse_knobs(&nr_knobs_cpu, &nr_knobs_node, add_knob);

    return 0;

fail:
    free_defaults();
    return -ENOMEM;

}


/* overrides the default kernel llseek function to prevent a not working pread/pwrite on certain kernel versions */
static loff_t x86_adapt_seek(struct file *file, loff_t offset, int orig)
{
    loff_t ret;
    struct inode *inode = file->f_mapping->host;

    mutex_lock(&inode->i_mutex);
    switch (orig) {
    case 0:
        file->f_pos = offset;
        ret = file->f_pos;
        break;
    case 1:
        file->f_pos += offset;
        ret = file->f_pos;
        break;
    default:
        ret = -EINVAL;
    }
    mutex_unlock(&inode->i_mutex);
    return ret;
}


/* returns from register reading the corresponding bits to the given bitmask */
__always_inline static u64 get_setting_from_register_reading(u64 register_reading, u64 bitmask) 
{
    int i;
    u64 ret_val = 0;
    ret_val = bitmask & register_reading;
    for (i = 0;i<64;i++) {
        if (bitmask&(1ULL<<i)) {
            break;
        }
    }
    if (i == 64)
        return 0;
    ret_val = ret_val>>i;
    return ret_val;
}

/* reads the setting of msr / pci knob */
static u64 read_setting(int dev_nr, struct knob_entry knob) 
{
    u64 register_reading = 0;
    u32 l = 0,h;
    /* switch device */
    if (dev_nr >= 0) {
        struct pci_dev * nb = NULL;
        switch (knob.device) {
            case MSR:
                if ( cpumask_equal(get_cpu_mask(dev_nr),&(current->cpus_allowed))) {
                    register_reading = native_read_msr(knob.register_index);
                }
                else {
                    rdmsr_on_cpu(dev_nr, knob.register_index,&l, &h);
                    register_reading = (l|(((u64)h)<<32));
                }
                break;
            case NB_F0:
                nb = nb_f0[dev_nr];
                break;
            case NB_F1:
                nb = nb_f1[dev_nr];
                break;
            case NB_F2:
                nb = nb_f2[dev_nr];
                break;
            case NB_F3:
                nb = nb_f3[dev_nr];
                break;
            case NB_F4:
                nb = nb_f4[dev_nr];
                break;
            case NB_F5:
                nb = nb_f5[dev_nr];
                break;
            case SB_PCU0:
                nb = sb_pcu0[dev_nr];
                break;
            case SB_PCU1:
                nb = sb_pcu1[dev_nr];
                break;
            case SB_PCU2:
                nb = sb_pcu2[dev_nr];
                break;
            case HSW_PCU0:
                nb = hsw_pcu0[dev_nr];
                break;
            case HSW_PCU1:
                nb = hsw_pcu1[dev_nr];
                break;
            case HSW_PCU2:
                nb = hsw_pcu2[dev_nr];
                break;
            case UNCORE:
                break;
        }
        if (nb) {
            pci_read_config_dword(nb,knob.register_index,&l);
            register_reading = l;
    }
    } else {
        printk(KERN_ERR "Failed to read setting of knob %s. Can not find device %d.\n", knob.name ,dev_nr);
    }
    return register_reading;
}

/* writes the setting to msr / pci knob */
static int write_setting(int dev_nr, struct knob_entry knob, u64 setting) 
{
    u32 l = 0,h = 0,i;
    u64 orig_setting;


    /* check whether setting is valid */
    for (i = 0;i<knob.restricted_settings_length;i++)
        if (setting == knob.restricted_settings[i])
          break;
    /* not one of the restricted settings -> return EINVAL */
    /* if there are restrictions and we didn't match the restriction */
    if (knob.restricted_settings_length && ( i == knob.restricted_settings_length ) )
      return -EINVAL;

    for (i = 0;i<knob.reserved_settings_length;i++)
        if (setting == knob.reserved_settings[i])
          return -EINVAL;


    /* read register */
    orig_setting = read_setting(dev_nr,knob);

    /* avoid a bitmask of 0 and prepare align setting according to bitmask */
    for (i = 0;i<64;i++) {
        if (knob.bitmask&(1ULL<<i))
            break;
    }
    if (i == 64)
        return -EPERM;
    /* align the setting according to the bitmask */
    setting = setting<<i;
    orig_setting &= (~knob.bitmask);
    orig_setting |= setting;
    /* split it in high and low (for MSRs) */
    l = orig_setting&(0xFFFFFFFF);
    h = orig_setting>>32;
    if (dev_nr >= 0) {
        struct pci_dev * nb = NULL;
        switch (knob.device) {
            /* write to msr */
            case MSR:
                if (cpumask_equal(get_cpu_mask(dev_nr),&(current->cpus_allowed))) {
                    native_write_msr(knob.register_index,l,h);
                }
                else {
                    wrmsr_on_cpu(dev_nr, knob.register_index,l, h);
                }
                break;
            case NB_F0:
                nb = nb_f0[dev_nr];
                break;
            case NB_F1:
                nb = nb_f1[dev_nr];
                break;
            case NB_F2:
                nb = nb_f2[dev_nr];
                break;
            case NB_F3:
                nb = nb_f3[dev_nr];
                break;
            case NB_F4:
                nb = nb_f4[dev_nr];
                break;
            case NB_F5:
                nb = nb_f5[dev_nr];
                break;
            case SB_PCU0:
                nb = sb_pcu0[dev_nr];
                break;
            case SB_PCU1:
                nb = sb_pcu1[dev_nr];
                break;
            case SB_PCU2:
                nb = sb_pcu2[dev_nr];
                break;
            case HSW_PCU0:
                nb = hsw_pcu0[dev_nr];
                break;
            case HSW_PCU1:
                nb = hsw_pcu1[dev_nr];
                break;
            case HSW_PCU2:
                nb = hsw_pcu2[dev_nr];
                break;
            case UNCORE:
                break;
        }
        /* write to PCI device */
        if (nb)
           pci_write_config_dword(nb,knob.register_index,l);
        return 0;
    } else {
        printk(KERN_ERR "Failed to write setting of knob %s. Can not find device %d.\n", knob.name ,dev_nr);
        return -ENXIO;
    }
}

static int reset_setting(int type, int dev_nr, struct knob_entry knob, u64 default_setting)
{
    if (knob.readonly)
        return 0;
    switch(type)
    {
        case X86_ADAPT_CPU:
                        return write_setting(dev_nr,knob,default_setting);
        case X86_ADAPT_NODE:
                        return write_setting(dev_nr,knob,default_setting);
        default:
            printk("Error, resetting unknown knob");
            return 1;
    }
} 

/* resolves the device from file and writes the buffer to the knob, that corresponds to ppos */
static ssize_t do_write(struct file * file, const char * buf, 
        size_t count, loff_t *ppos, int type,
        struct knob_entry * entries, u32 entries_length, u64 ** defaults)
{        /*
          * If file position is non-zero, then assume the string has
          * been read and indicate there is no more data to be read.
          */

    char kernel_buffer[8];

    struct dentry * de = ((file->f_path).dentry);
    /* get device nr */
    int dev_nr = 0;
    char* end;

    /* initial checks whether the arguments are valid */
    if (count != 8)
        return -ENXIO;
    if (copy_from_user(kernel_buffer,buf,8)!=0)
        return -ENXIO;

    dev_nr = simple_strtol(de->d_name.name,&end,10);
    if (*end) {
        /* special handling, change all settings */
        if (!strcmp(de->d_name.name,"all"))
            dev_nr = -1;
        else
            return -ENXIO;
    } 
    

    if ((*ppos < entries_length)) {
        u64 setting = 0;
        if (entries[*ppos].readonly)
            return -EPERM;
        setting = ((uint64_t*)kernel_buffer)[0];
        if (dev_nr == -1) {
            /* if reset */
            if ( *ppos == KNOB_RESET )
            {
                 /* reset to default */
                 int ret = 0;
                 int i;
                 int cpu_or_node;
                 switch (type)
                 {
                     case X86_ADAPT_CPU:
                         for_each_online_cpu(cpu_or_node) {
                             for ( i = 1 ; i < entries_length ; i++ )
                             {
                                 ret = reset_setting(X86_ADAPT_CPU,cpu_or_node,entries[i],defaults[cpu_or_node][i-1]);
                                 /* if error, return error */
                                 if (ret) return ret;
                             }
                         }
                         break;
                     case X86_ADAPT_NODE:
                          for_each_online_node(cpu_or_node) {
                              for ( i = 1 ; i < entries_length ; i++ )
                              {
                                  ret = reset_setting(X86_ADAPT_NODE,cpu_or_node,entries[i],defaults[cpu_or_node][i-1]);
                                  /* if error, return error */
                                  if (ret) return ret;
                              }
                          }
                          break;
                      default:
                          printk("Invalid reset");
                 }
                 return count;
            }
            else
            {
                /* not reset, but write */
                /* write to all devices */
                if (type == X86_ADAPT_CPU) {
                    int i;
                    for_each_online_cpu(i) {
                        int ret = write_setting(i,entries[*ppos],setting);
                        if(ret)
                            return ret;
                    }
                } else {
                    int i;
                    for_each_online_node(i) {
                        int ret = write_setting(i,entries[*ppos],setting);
                        if(ret)
                            return ret;
                    }
               }
            }
            return count;
        } else {
            int ret=0;
            /* if reset */
            if ( *ppos == KNOB_RESET )
            {
                 /* only if s.o. writes 0 */
                 /* reset to default */
                 int i;
                 for ( i = 1 ; i < entries_length ; i++ )
                 {
                     ret = reset_setting(type, dev_nr, entries[i], defaults[dev_nr][i-1]);
                     /* if error, return error */
                     if (ret) return ret;
                 }
                 return count;
            }
            else
            {
                /* not reset, but write */
                ret = write_setting(dev_nr,entries[*ppos],setting);
                if (ret) return ret;
                return count;
            }
        }
    } else {
        return -ENXIO;
    }
}

static ssize_t cpu_write(struct file * file, const char * buf, 
        size_t count, loff_t *ppos)
{
    return do_write(file,buf,count,ppos,X86_ADAPT_CPU,active_knobs_cpu,active_knobs_cpu_length,defaults_cpu);
}

static ssize_t node_write(struct file * file, const char * buf, 
        size_t count, loff_t *ppos)
{
    return do_write(file,buf,count,ppos,X86_ADAPT_NODE,active_knobs_node,active_knobs_node_length,defaults_node);
}

/* resolves device from file and reads the knob, that corresponds to ppos into the buffer */
static ssize_t do_read(struct file * file, char * buf, 
        size_t count, loff_t *ppos, int type,
        struct knob_entry * entries, u32 entries_length)
{

    /*
     * If file position is non-zero, then assume the string has
     * been read and indicate there is no more data to be read.
     */
    struct dentry * de = ((file->f_path).dentry);
    /* get device nr */
    int dev_nr = 0;
    char* end;
    u64 setting = 0;

    /* can not read reset */
    if ( *ppos == KNOB_RESET )
    {
        if (copy_to_user(buf, &setting, 8)) {
            return -EFAULT;
        }
        return 8;
    }

    dev_nr = simple_strtol(de->d_name.name,&end,10);
    if (*end) {
        if (strcmp(de->d_name.name,"all"))
            /* special handling, change all settings */
            return -ENXIO;
        else
            dev_nr = -1;
    } 

    /* print value for id */
    if (*ppos<entries_length) {

        /* setting read from nb / msr */
        u64 tmp = 0;

        /* not enough buffer */
        if (count != 8) {
            // printk("Not enough buffer %zu %u\n",count,8);
            return -EFAULT;
        }

        /* read it */
        if (dev_nr == -1) {
            /* read all devices */
            if (type == X86_ADAPT_CPU) {
                int i;
                for_each_online_cpu(i) {
                    tmp = read_setting(i,entries[*ppos]);
                    setting |= tmp;
                }
            } else {
                int i;
                for_each_online_node(i) {
                    tmp = read_setting(i,entries[*ppos]);
                    setting |= tmp;
                }
            }
        } else {
            setting = read_setting(dev_nr,entries[*ppos]);
        }
        /* shift it */
        setting = get_setting_from_register_reading(setting, entries[*ppos].bitmask);
        /* to user buffer */
        if (copy_to_user(buf, &setting, 8)) {
            return -EFAULT;
        }
        //((u64 *)buf)[0] = setting;
        return 8;
    } else {
        // printk(KERN_ERR "Wrong id\n");
        return -ENXIO;
    }
}

static ssize_t cpu_read(struct file * file, char * buf, 
        size_t count, loff_t *ppos)
{
    return do_read(file,buf,count,ppos,X86_ADAPT_CPU,active_knobs_cpu,active_knobs_cpu_length);
}

static ssize_t node_read(struct file * file, char * buf, 
        size_t count, loff_t *ppos)
{
    return do_read(file,buf,count,ppos,X86_ADAPT_NODE,active_knobs_node,active_knobs_node_length);
}

/* writes information about all cpu/node knobs to the buffer */
static ssize_t get_entry_defs(char * buf, size_t count, loff_t *ppos,
        struct knob_entry * defs, u32 length)
{
    if (*ppos == 0) {
        /* write explanation about all available knobs to buffer */
        /* but first, we write 4 byte of total length */
        int position = 0;
        int len,i;
        /* This is the length of the definition data in byte
         * The first 4 byte contain the overall length of the definitions */
        u32 total_length = 4;
        /* we need at least 4 byte to tell the user how long the information data is */
        if (count<4) {
            return -ENOMEM;
        }
        /* the total length needed to store all the knob infos is the most information
         * thus, it has to be written before everything else.
         * Even if the read of this file is only 4 bytes long, */
        for (i = 0;i<length;i++) {
            /* For every knob, we need the header, the name and the description */
            total_length += ENTRY_HEADER_SIZE;
            len = strlen(defs[i].name);
            total_length += len;
            len = strlen(defs[i].description);
            total_length += len;
        }

        /* write total theoretical string length to buf*/
        if (copy_to_user(buf,&total_length,4))
          return -EFAULT;

        /* position in the userspace buffer */
        position = 4;
        /* write all the definitions */
        for (i = 0;i<length;i++) {
            /* if the current item doesn't fit in the userspace buffer, break. */
            if ((position+ENTRY_HEADER_SIZE+strlen(defs[i].name)+
                strlen(defs[i].description))>count)
                break;
            if (copy_to_user(&buf[position],&i,4))
                return -EFAULT;
            position += 4;
            if (copy_to_user(&buf[position],&defs[i].length,1))
              return -EFAULT;
            position += 1;
            len = strlen(defs[i].name);
            if (copy_to_user(&buf[position],&len,4))
              return -EFAULT;
            position += 4;
            if (copy_to_user(&buf[position],defs[i].name,len))
              return -EFAULT;
            position += len;
            len = strlen(defs[i].description);
            if (copy_to_user(&buf[position],&len,4))
              return -EFAULT;
            position += 4;
            if (copy_to_user(&buf[position],defs[i].description,len))
              return -EFAULT;
            position += len;
        }
        *ppos = position;
        return position;
    } else {
        return 0;
    }
}

static ssize_t def_node_read(struct file * file, char * buf, size_t count, loff_t *ppos)
{
    return get_entry_defs(buf,count,ppos,active_knobs_node,active_knobs_node_length);
}

static ssize_t def_cpu_read(struct file * file, char * buf, size_t count, loff_t *ppos)
{
    return get_entry_defs(buf,count,ppos,active_knobs_cpu,active_knobs_cpu_length);
}


static const struct file_operations x86_adapt_cpu_fops = {
    .owner                = THIS_MODULE,
    .llseek               = x86_adapt_seek,
    .read                 = cpu_read,
    .write                = cpu_write,
};

static const struct file_operations x86_adapt_node_fops = {
    .owner                = THIS_MODULE,
    .llseek               = x86_adapt_seek,
    .read                 = node_read,
    .write                = node_write,
};

static const struct file_operations x86_adapt_def_cpu_fops = {
    .owner                = THIS_MODULE,
    .read                 = def_cpu_read,
};

static const struct file_operations x86_adapt_def_node_fops = {
    .owner                = THIS_MODULE,
    .read                 = def_node_read,

};

static int x86_adapt_device_create(int cpu)
{
    struct device *dev;

    dev = device_create(x86_adapt_class,NULL,MKDEV(MAJOR(x86_adapt_cpu_device), 
                MINOR(x86_adapt_cpu_device)+cpu),NULL,"cpu%d",cpu);

    return IS_ERR(dev) ? PTR_ERR(dev) : 0;
}

/* function to manage cpu hotplug events */
static int x86_adapt_cpu_callback(struct notifier_block *nfb,
                                unsigned long action, void *hcpu)
{
    unsigned int cpu = (unsigned long) hcpu;
    int err = 0;

    switch (action) {
        case CPU_ONLINE:
        case CPU_ONLINE_FROZEN:
            err = x86_adapt_device_create(cpu);
            break;
        case CPU_DEAD:
        case CPU_DEAD_FROZEN:
            device_destroy(x86_adapt_class,MKDEV(MAJOR(x86_adapt_cpu_device),
                MINOR(x86_adapt_cpu_device)+cpu));
            break;
    }
    return notifier_from_errno(err);
}

static struct notifier_block x86_adapt_cpu_notifier __refdata = {
    .notifier_call = x86_adapt_cpu_callback,
};

/* this function is used to generate the path in /dev 
 * for the corresponding /sys/class/x86_adapt entries
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0)
static char* x86_adapt_devnode(struct device *dev, umode_t *mode)
#else
static char* x86_adapt_devnode(struct device *dev, mode_t *mode)
#endif
{
    int min = MINOR(dev->devt);
    int max = MAJOR(dev->devt);

    if (max == MAJOR(x86_adapt_cpu_device)) {
      if(mode)
          *mode = S_IRUGO | S_IWUSR | S_IWGRP;
        if (min == num_possible_cpus())
            return kasprintf(GFP_KERNEL, "x86_adapt/cpu/all");
        else 
            return kasprintf(GFP_KERNEL, "x86_adapt/cpu/%u", min);
    } else if (max == MAJOR(x86_adapt_node_device)) {
      if(mode)
          *mode = S_IRUGO | S_IWUGO;
        if (min == num_possible_nodes())
            return kasprintf(GFP_KERNEL, "x86_adapt/node/all");
        else
            return kasprintf(GFP_KERNEL, "x86_adapt/node/%u", min);
    } else if (max == MAJOR(x86_adapt_def_cpu_device)) {
      if(mode)
          *mode = S_IRUGO;
        return kasprintf(GFP_KERNEL, "x86_adapt/cpu/definitions");
    } else { /* MAJOR(x86_adapt_def_node_device) */
      if(mode)
          *mode = S_IRUGO;
        return kasprintf(GFP_KERNEL, "x86_adapt/node/definitions");
    }
}


static int __init x86_adapt_init(void)
{
    int i,err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
    cpu_notifier_register_begin();
#endif
    get_online_cpus();
    if ((x86_adapt_class = class_create(THIS_MODULE, "x86_adapt")) == NULL) {
        printk(KERN_ERR "Failed to create sysfs class\n");
        err = -1;
        goto fail;
    }
    x86_adapt_class->devnode = x86_adapt_devnode;
    ALLOC_AND_INIT(x86_adapt_cpu, DEVICE_CREATE, cpu);
    ALLOC_AND_INIT(x86_adapt_node, DEVICE_CREATE, node);
    ALLOC_AND_INIT(x86_adapt_def_cpu, DEF_CREATE, cpu);
    ALLOC_AND_INIT(x86_adapt_def_node, DEF_CREATE, node);
    err = buildup_entries();
    if (err) {
        printk(KERN_ERR "Failed to allocate memory for cpu/node knobs\n");
        goto fail;
    }
    /* Based on BKDG */
    ALLOC_NB_PCI(0);
    ALLOC_NB_PCI(1);
    ALLOC_NB_PCI(2);
    ALLOC_NB_PCI(3);
    ALLOC_NB_PCI(4);
    ALLOC_NB_PCI(5);

    /* Based on:
     * Intel Xeon Processor E5 Product Family Datasheet- Volume Two: Registers
     * Intel Xeon Processor E5 v2 Product Family Datasheet- Volume Two: Registers */
    ALLOC_UNCORE_PCI(sb_pcu0,10,0);
    ALLOC_UNCORE_PCI(sb_pcu1,10,1);
    ALLOC_UNCORE_PCI(sb_pcu2,10,2);
    ALLOC_UNCORE_PCI(hsw_pcu0,30,0);
    ALLOC_UNCORE_PCI(hsw_pcu1,30,1);
    ALLOC_UNCORE_PCI(hsw_pcu2,30,2);

    read_defaults();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
    __register_hotcpu_notifier(&x86_adapt_cpu_notifier);
    put_online_cpus();
    cpu_notifier_register_done();
#else
    register_hotcpu_notifier(&x86_adapt_cpu_notifier);
    put_online_cpus();
#endif
    printk(KERN_INFO "Succesfully Started x86 Adapt Processor Feature Device Driver\n");
    return 0;

fail:
    UNREGISTER_AND_DELETE(x86_adapt_cpu, DEVICE_DESTROY, cpu);
    UNREGISTER_AND_DELETE(x86_adapt_node, DEVICE_DESTROY, node);
    UNREGISTER_AND_DELETE(x86_adapt_def_cpu, DEF_DESTROY, cpu);
    UNREGISTER_AND_DELETE(x86_adapt_def_node, DEF_DESTROY, node);
    if (active_knobs_cpu) {
        kfree(active_knobs_cpu);
        active_knobs_cpu = NULL;
    }
    if (active_knobs_node) {
        kfree(active_knobs_node);
        active_knobs_node = NULL;
    }
    FREE_NB(0);
    FREE_NB(1);
    FREE_NB(2);
    FREE_NB(3);
    FREE_NB(4);
    FREE_NB(5);
    FREE_PCI(sb_pcu0);
    FREE_PCI(sb_pcu1);
    FREE_PCI(sb_pcu2);
    FREE_PCI(hsw_pcu0);
    FREE_PCI(hsw_pcu1);
    FREE_PCI(hsw_pcu2);
    free_defaults();

    put_online_cpus();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
    cpu_notifier_register_done();
#endif
    return err;
}

static void __exit x86_adapt_exit(void)
{
    int i;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
    cpu_notifier_register_begin();
#endif
    get_online_cpus();
    UNREGISTER_AND_DELETE(x86_adapt_cpu, DEVICE_DESTROY, cpu);
    UNREGISTER_AND_DELETE(x86_adapt_node, DEVICE_DESTROY, node);
    UNREGISTER_AND_DELETE(x86_adapt_def_cpu, DEF_DESTROY, cpu);
    UNREGISTER_AND_DELETE(x86_adapt_def_node, DEF_DESTROY, node);
    if (active_knobs_cpu) {
        kfree(active_knobs_cpu);
        active_knobs_cpu = NULL;
    }
    if (active_knobs_node) {
        kfree(active_knobs_node);
        active_knobs_node = NULL;
    }
    FREE_NB(0);
    FREE_NB(1);
    FREE_NB(2);
    FREE_NB(3);
    FREE_NB(4);
    FREE_NB(5);
    FREE_PCI(sb_pcu0);
    FREE_PCI(sb_pcu1);
    FREE_PCI(sb_pcu2);
    FREE_PCI(hsw_pcu0);
    FREE_PCI(hsw_pcu1);
    FREE_PCI(hsw_pcu2);
    free_defaults();

    class_destroy(x86_adapt_class);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
    __unregister_hotcpu_notifier(&x86_adapt_cpu_notifier);
    put_online_cpus();
    cpu_notifier_register_done();
#else
    unregister_hotcpu_notifier(&x86_adapt_cpu_notifier);
    put_online_cpus();
#endif
    printk(KERN_INFO "Shutting Down x86 Adapt Processor Feature Device Driver\n");
}

module_init(x86_adapt_init);
module_exit(x86_adapt_exit);

MODULE_AUTHOR("Robert Schoene <robert.schoene@tu-dresden.de>");
MODULE_AUTHOR("Michael Werner <michael.werner3@tu-dresden.de>");
MODULE_AUTHOR("Joseph Schuchart <joseph.schuchart@tu-dresden.de>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("x86 Adapt Processor Feature Device Driver");
MODULE_VERSION("0.3");
