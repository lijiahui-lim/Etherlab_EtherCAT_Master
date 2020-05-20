/*
 * ethercat_wrapper.c
 */

#include "ethercat_wrapper.h"
#include "slave.h"
#include "ethercat_wrapper_slave.h"

#include <ecrt.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#define SDO_REQUEST_TIMEOUT     50  // ms

#ifndef VERSIONING
#error no version information
#endif

#define XSTR(a)      STR(a)
#define STR(a)       #a

const char *g_version = "v" XSTR(VERSIONING);

#define LIBETHERCAT_WRAPPER_SYSLOG "libethercat_wrapper"

#if 0 /* draft */
struct _pdo_memory {
  uint8_t *processdataa;
  uiint32_t **offset;  ////< lsit of uint32_T pointer to individual pdo offsets.
};
#endif

static void update_domain_state(Ethercat_Master_t *master);
static void update_master_state(Ethercat_Master_t *master);
static void update_all_slave_state(Ethercat_Master_t *master);

static void free_all_slaves(Ethercat_Master_t *master);

const char *ecw_master_get_version(void)
{
  return g_version;
}

static int setup_sdo_request(Ethercat_Slave_t *slave)
{
  for (size_t sdo_index = 0; sdo_index < slave->sdo_count; sdo_index++) {
    Sdo_t *sdo = slave->dictionary + sdo_index;

    if (sdo->bit_length >= 8) {
      sdo->upload_request = ecrt_slave_config_create_sdo_request(
          slave->config, sdo->index, sdo->subindex, (sdo->bit_length / 8));

      sdo->download_request = ecrt_slave_config_create_sdo_request(
          slave->config, sdo->index, sdo->subindex, (sdo->bit_length / 8));
    } else {
      sdo->upload_request = ecrt_slave_config_create_sdo_request(
          slave->config, sdo->index, sdo->subindex, 1);

      sdo->download_request = ecrt_slave_config_create_sdo_request(
        slave->config, sdo->index, sdo->subindex, 1);
    }

    if (sdo->upload_request == NULL || sdo->download_request == NULL) {
      syslog(LOG_ERR,
             "Warning, could not create SDO requests for cyclic operation!");
      return -1;
    } else {
      ecrt_sdo_request_timeout(sdo->upload_request, SDO_REQUEST_TIMEOUT);
      sdo->upload_request_state = ecrt_sdo_request_state(sdo->upload_request);

      ecrt_sdo_request_timeout(sdo->download_request, SDO_REQUEST_TIMEOUT);
      sdo->download_request_state = ecrt_sdo_request_state(sdo->download_request);
    }
  }

  return 0;
}

/*
 * populate the fields:
 * master->slave[*]->[RT]xPDO
 *
 * FIXME Move to slave.c:ecw_slave_scan()
 */
static int slave_config(Ethercat_Master_t *master, Ethercat_Slave_t *slave)
{
  slave->sdo_count = slave->info->sdo_count;
  if (slave->sdo_count == 0) {
    syslog(LOG_WARNING, "Slave %d has no SDOs", slave->info->position);
  }

  slave->type = type_map_get_type(slave->info->vendor_id,
                                  slave->info->product_code);

  slave->out_pdo_count = 0;
  slave->in_pdo_count = 0;

  /* add one more field for the sync manager count because of the last element */
  slave->sm_info = malloc(
      (slave->info->sync_count + 1) * sizeof(ec_sync_info_t));

  for (int j = 0; j < slave->info->sync_count; j++) {
    ec_sync_info_t *sm_info = slave->sm_info + j;
    ecrt_master_get_sync_manager(master->master, slave->info->position, j, sm_info);

    /* if there is no PDO, this SyncManager is for mailbox communication */
    if (sm_info->n_pdos == 0)
      continue;

    if (sm_info->pdos == NULL) {
      //syslog(LOG_ERR, "Warning, slave not configured");
      sm_info->pdos = malloc(sm_info->n_pdos * sizeof(ec_pdo_info_t));
    }

    for (unsigned int k = 0; k < sm_info->n_pdos; k++) {
      ec_pdo_info_t *pdo_info = sm_info->pdos + k;
      ecrt_master_get_pdo(master->master, slave->info->position, j, k, pdo_info);

      if (pdo_info->entries == NULL) {
        //syslog(LOG_ERR, "Warning pdo_info.entries is NULL!");
        pdo_info->entries = malloc(pdo_info->n_entries * sizeof(ec_pdo_entry_info_t));
      }

      if (sm_info->dir == EC_DIR_OUTPUT) {
        slave->out_pdo_count += pdo_info->n_entries;
      } else if (sm_info->dir == EC_DIR_INPUT) {
        slave->in_pdo_count += pdo_info->n_entries;
      } else {
        /* FIXME error handling? */
        syslog(LOG_ERR, "WARNING undefined direction");
      }

      for (unsigned int l = 0; l < pdo_info->n_entries; l++) {
        ec_pdo_entry_info_t *pdo_entry = pdo_info->entries + l;
        ecrt_master_get_pdo_entry(master->master, slave->info->position, j, k,
                                  l, pdo_entry);
      }
    }
  }

  // Allocate the required memory for all PDO values
  slave->output_values = calloc(slave->out_pdo_count, sizeof(pdo_t));
  slave->input_values = calloc(slave->in_pdo_count, sizeof(pdo_t));

  /* Add the last pivot element to the list of sync managers, this is
   * necessary within the etherlab master when the PDOs are configured. */
  ec_sync_info_t *sm_info = slave->sm_info + slave->info->sync_count;
  *sm_info = (ec_sync_info_t ) { 0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT };

  slave->config = ecrt_master_slave_config(master->master, slave->reference_alias,
                                           slave->relative_position,
                                           slave->info->vendor_id,
                                           slave->info->product_code);

  if (slave->config == NULL) {
    syslog(LOG_ERR, "Error acquiring slave configuration");
    return -1;
  }

  if (ecrt_slave_config_pdos(slave->config, EC_END, slave->sm_info)) {
    syslog(LOG_ERR, "Error, failed to configure PDOs");
    return -1;
  }

  /*
   * read slave object dictionary and get the values
   */

  /* count the real number of object entries (including subindexes) */
  size_t object_count = slave->sdo_count;
  for (size_t i = 0; i < slave->sdo_count; i++) {
    ec_sdo_info_t sdoi;
    if (ecrt_sdo_info_get(master->master, slave->info->position, i, &sdoi)) {
      syslog(
          LOG_ERR,
          "Error, unable to retrieve information of object dictionary info %ld",
          i);
      return -1;
    }

    object_count += sdoi.maxindex;
  }

  if (slave->sdo_count && slave->sdo_count == object_count) {
    syslog(LOG_WARNING, "All Slave %d SDOs have no index", slave->info->position);
  }

  slave->dictionary = malloc(/*slave->sdo_count */object_count * sizeof(Sdo_t));

  for (size_t i = 0, current_sdo = 0; i < slave->sdo_count; i++) {
    ec_sdo_info_t sdoi;
    //size_t current_sdo = 0;

    if (ecrt_sdo_info_get(master->master, slave->info->position, i, &sdoi)) {
      syslog(
          LOG_WARNING,
          "Warning, unable to retrieve information of object dictionary entry %ld",
          i);
      continue;
    }

    for (int j = 0; j < (sdoi.maxindex + 1); j++) {
      Sdo_t *sdo = slave->dictionary + current_sdo++;
      ec_sdo_info_entry_t entry;

      if (ecrt_sdo_get_info_entry(master->master, slave->info->position,
                                  sdoi.index, j, &entry)) {
        syslog(LOG_WARNING,
               "Warning, cannot read SDO entry index: 0x%04x subindex: %d",
               sdoi.index, j);
        continue;
      }

      sdo->index = sdoi.index;
      sdo->subindex = j;
      sdo->entry_type = entry.data_type;
      sdo->object_type = sdoi.object_code;
      sdo->bit_length = entry.bit_length;
      sdo->value = 0;
      sdo->value_string[0] = '\0';

      memmove(sdo->name, entry.description, EC_MAX_STRING_LENGTH);
      memmove(sdo->object_name, sdoi.name, EC_MAX_STRING_LENGTH);
      memmove(sdo->read_access, entry.read_access, EC_SDO_ENTRY_ACCESS_COUNTER);
      memmove(sdo->write_access, entry.write_access,
      EC_SDO_ENTRY_ACCESS_COUNTER);

      /* SDO requests are uploaded at master_start(), they are only
       * needed when master and slave are in real time context. */
      sdo->upload_request = NULL;
      sdo->download_request = NULL;
    }
  }

  slave->sdo_count = object_count;

  return 0;
}

void ecw_print_topology(Ethercat_Master_t *master)
{
  openlog(LIBETHERCAT_WRAPPER_SYSLOG, LOG_CONS | LOG_PID | LOG_NDELAY,
  LOG_USER);

  ec_slave_info_t *slave_info;

  for (size_t i = 0; i < master->slave_count; i++) {
    (master->slaves + i)->info = malloc(sizeof(ec_slave_info_t));
    slave_info = (master->slaves + i)->info;

    if (ecrt_master_get_slave(master->master, i, slave_info) != 0) {
      syslog(LOG_DEBUG,
             "[DEBUG %s] Couldn't read slave configuration on position %ld",
             __func__, i);
    }

    printf("[DEBUG] slave count: %ld ::\n", i);
    printf("        Position: %d\n", slave_info->position);
    printf("        Vendor ID: 0x%08x\n", slave_info->vendor_id);
    printf("        Number of SDOs: %d\n", slave_info->sdo_count);

    Ethercat_Slave_t *slave = master->slaves + i;

    printf("\nDEBUG Output\n-------------\n");
    printf("Slave index: %d\n", slave->info->position);
    printf("      type:  %s\n", ecw_slave_type_string(slave->type));
    printf("  # Sync manager: %d\n", slave->info->sync_count);

    printf("  out PDO count: %lu\n", slave->out_pdo_count);
    printf("  in  PDO count: %lu\n", slave->in_pdo_count);

    for (uint8_t i = 0; i < slave->info->sync_count; i++) {
      ec_sync_info_t *sm_info = slave->sm_info + i;

      printf("| Slave: %d, Sync manager: %d\n", slave->info->position, i);
      printf("|    index: 0x%04x\n", sm_info->index);
      printf("|    direction: %d\n", sm_info->dir);
      printf("|    # of PDOs: %d\n", sm_info->n_pdos);

      if (sm_info->n_pdos == 0) {
        fprintf(stdout, "[INFO] no PDOs to assign... continue \n");
        continue;
      }

      for (unsigned int j = 0; j < sm_info->n_pdos; j++) {
        ec_pdo_info_t *pdo_info = sm_info->pdos + j;
        if (pdo_info == NULL) {
          syslog(LOG_ERR, "ERROR PDO info is not available");
        }

        printf("|    | PDO Info (%d):\n", j);
        printf("|    | PDO Index: 0x%04x;\n", pdo_info->index);
        printf("|    | # of Entries: %d\n", pdo_info->n_entries);

        for (unsigned int k = 0; k < pdo_info->n_entries; k++) {
          ec_pdo_entry_info_t *entry = pdo_info->entries + k;

          printf("|    |   | Entry %d: 0x%04x:%d (%d)\n", k, entry->index,
                 entry->subindex, entry->bit_length);
        }
      }
    }
  }

  closelog();
}

void ecw_print_domain_regs(Ethercat_Master_t *master)
{
  ec_pdo_entry_reg_t *domain_reg_cur = master->domain_reg;

  printf("Domain Registrations:\n");
  while (domain_reg_cur->vendor_id != 0) {
    printf("  { %d, %d, 0x%04x, 0x%04x, 0x%02x, %d, 0x%x, 0x%x  },\n",
           domain_reg_cur->alias, domain_reg_cur->position,
           domain_reg_cur->vendor_id, domain_reg_cur->product_code,
           domain_reg_cur->index, domain_reg_cur->subindex,
           *(domain_reg_cur->offset), *(domain_reg_cur->bit_position));

    domain_reg_cur++;
  }
}

void ecw_print_all_slave_od(Ethercat_Master_t *master)
{
  Ethercat_Slave_t *slave = NULL;
  Sdo_t *sdo = NULL;

  for (size_t k = 0; k < master->slave_count; k++) {
    slave = master->slaves + k;
    printf("[DEBUG] Slave %d, number of SDOs: %lu\n", slave->info->position,
           ecw_slave_get_sdo_count(slave));

    for (size_t i = 0; i < slave->sdo_count; i++) {
      sdo = slave->dictionary + i;

      printf("    +-> Object Number: %ld ", i);
      printf(", 0x%04x:%d", sdo->index, sdo->subindex);
      printf(", %ld, %d", sdo->value, sdo->bit_length);
      printf(", %d", sdo->object_type);
      printf(", %d", sdo->entry_type);
      printf(", \"%s\"\n", sdo->name);
    }
  }
}

int ecw_preemptive_master_rescan(int master_id)
{
  ec_master_t *master = ecrt_open_master(master_id);

  if (master == NULL) {
    return -1;
  }

  int result = ecrt_master_rescan(master);

  ecrt_release_master(master);

  return result;
}

ec_master_info_t* ecw_preemptive_master_info(int master_id)
{
  ec_master_t *master = ecrt_open_master(master_id);

  if (master == NULL) {
    return NULL;
  }

  int timeout = 1000;
  ec_master_info_t *info = calloc(1, sizeof(ec_master_info_t));
  info->scan_busy = 1;
  info->link_up = 0;
  while (info->link_up == 0 && info->scan_busy && (timeout-- > 0)) {
    ecrt_master(master, info);
    usleep(1000);
  }

  ecrt_release_master(master);

  if (timeout <= 0) {
    syslog(LOG_ERR, "ERROR, link_up or scan_busy timed out");
    return NULL;
  }

  return info;
}

int ecw_preemptive_slave_count(int master_id)
{
  ec_master_info_t *info = ecw_preemptive_master_info(master_id);

  if (info == NULL) {
    return -1;
  }

  int slave_count = info->slave_count;

  free(info);

  return slave_count;
}

int ecw_preemptive_slave_index_check(int master_id, unsigned int slave_index)
{
  // Check if the slave index is valid
  ec_master_info_t *info = ecw_preemptive_master_info(master_id);

  if (info == NULL) {
    return 0;
  }

  if (slave_index >= info->slave_count) {
    syslog(LOG_ERR, "ERROR, invalid slave index");
    return 0;
  }

  free(info);

  return 1;
}

int ecw_preemptive_slave_sdo_count(int master_id, unsigned int slave_index)
{
  if (!ecw_preemptive_slave_index_check(master_id, slave_index)) {
    return -1;
  }

  // Get the slave info
  ec_master_t *master = ecrt_open_master(master_id);

  if (master == NULL) {
    return -1;
  }

  ec_slave_info_t *slave_info = malloc(sizeof(ec_slave_info_t));
  if (ecrt_master_get_slave(master, slave_index, slave_info) != 0) {
    syslog(LOG_ERR, "Error, could not read slave configuration for slave %d",
           slave_index);
    return -1;
  }

  ecrt_release_master(master);

  int sdo_count = slave_info->sdo_count;

  free(slave_info);

  return sdo_count;
}

int ecw_preemptive_slave_state(int master_id, unsigned int slave_index)
{
  if (!ecw_preemptive_slave_index_check(master_id, slave_index)) {
    return -1;
  }

  // Get the slave info
  ec_master_t *master = ecrt_open_master(master_id);

  if (master == NULL) {
    return -1;
  }

  ec_slave_info_t *slave_info = malloc(sizeof(ec_slave_info_t));
  if (ecrt_master_get_slave(master, slave_index, slave_info) != 0) {
    syslog(LOG_ERR, "Error, could not read slave configuration for slave %d",
           slave_index);
    return -1;
  }

  ecrt_release_master(master);

  int al_state = slave_info->al_state;

  free(slave_info);

  return al_state;
}

unsigned int ecw_preemptive_slave_vendor_id(int master_id,
                                            unsigned int slave_index)
{
  if (!ecw_preemptive_slave_index_check(master_id, slave_index)) {
    return 0;
  }

  // Get the slave info
  ec_master_t *master = ecrt_open_master(master_id);

  if (master == NULL) {
    return 0;
  }

  ec_slave_info_t *slave_info = malloc(sizeof(ec_slave_info_t));
  if (ecrt_master_get_slave(master, slave_index, slave_info) != 0) {
    syslog(LOG_ERR, "Error, could not read slave configuration for slave %d",
           slave_index);
    return 0;
  }

  ecrt_release_master(master);

  unsigned int vendor_id = slave_info->vendor_id;

  free(slave_info);

  return vendor_id;
}

unsigned int ecw_preemptive_slave_product_code(int master_id,
                                               unsigned int slave_index)
{
  if (!ecw_preemptive_slave_index_check(master_id, slave_index)) {
    return 0;
  }

  // Get the slave info
  ec_master_t *master = ecrt_open_master(master_id);

  if (master == NULL) {
    return 0;
  }

  ec_slave_info_t *slave_info = malloc(sizeof(ec_slave_info_t));
  if (ecrt_master_get_slave(master, slave_index, slave_info) != 0) {
    syslog(LOG_ERR, "Error, could not read slave configuration for slave %d",
           slave_index);
    return 0;
  }

  ecrt_release_master(master);

  unsigned int product_code = slave_info->product_code;

  free(slave_info);

  return product_code;
}

Ethercat_Master_t *ecw_master_init(int master_id)
{
  openlog(LIBETHERCAT_WRAPPER_SYSLOG, LOG_CONS | LOG_PID | LOG_NDELAY,
  LOG_USER);

  Ethercat_Master_t *master = malloc(sizeof(Ethercat_Master_t));
  if (master == NULL) {
    /* Cannot allocate master */
    return NULL;
  }

  master->master = ecrt_request_master(master_id);
  if (master->master == NULL) {
    free(master);
    return NULL;
  }

  /* wait for the master and get its state */
  int timeout = 1000;
  ec_master_state_t state;
  state.link_up = 0;
  while (state.link_up == 0 && (timeout-- > 0)) {
    ecrt_master_state(master->master, &state);
    usleep(1000);
  }

  if (timeout <= 0) {
    syslog(LOG_ERR, "ERROR, link_state timed out");
    ecrt_release_master(master->master);
    free(master);
    return NULL;
  }

  /* configure slaves */
  ec_master_info_t *info = calloc(1, sizeof(ec_master_info_t));

  timeout = 1000;
  info->scan_busy = 1;
  while ((info->scan_busy) && (timeout-- > 0)) {
    ecrt_master(master->master, info);
    usleep(1000);
  }

  if (timeout <= 0) {
    syslog(LOG_ERR, "ERROR, scan_busy timed out");
    free(info);
    ecrt_release_master(master->master);
    free(master);
    return NULL;
  }

  if (info->slave_count != state.slaves_responding) {
    syslog(LOG_ERR, "ERROR, slave_count - slaves_responding mismatch");
    free(info);
    ecrt_release_master(master->master);
    free(master);
    return NULL;
  }

  master->slave_count = info->slave_count;
  master->slaves = malloc(master->slave_count * sizeof(Ethercat_Slave_t));

  size_t all_pdo_count = 0;

  /**
   * IMPORTANT NOTE
   *
   * Save the reference alias of each slave (not available in slave->info since
   * it keeps track only of slaves that have had their alias set and not of the
   * last set alias their position is relative to), as well as its position
   * relative to that alias. There are several EtherCAT master library functions
   * that require these two parameters:
   *   1) ecrt_master_slave_config
   *   2) ecrt_domain_reg_pdo_entry_list - through domain_reg parameter
   */
  uint16_t reference_alias = 0;
  uint16_t relative_position = 0;

  for (unsigned int i = 0; i < info->slave_count; i++) {
    Ethercat_Slave_t *slave = master->slaves + i;
    slave->master = master->master;
    slave->info = malloc(sizeof(ec_slave_info_t));
    if (ecrt_master_get_slave(master->master, i, slave->info) != 0) {
      syslog(LOG_ERR, "Error, could not read slave configuration for slave %d",
             i);
      free(info);
      ecw_master_release(master);
      return NULL;
    }

    if (slave->info->alias) {
      reference_alias = slave->info->alias;
      relative_position = 0;
    }

    slave->reference_alias = reference_alias;
    slave->relative_position = relative_position;

    /* get the PDOs from the buffered sync managers */
    if (slave_config(master, slave) != 0) {
      syslog(LOG_ERR, "ERROR, configuration slave %d", i);
      free(info);
      ecw_master_release(master);
      return NULL;
    }

    all_pdo_count += ((master->slaves + i)->out_pdo_count + (master->slaves + i)->in_pdo_count);

    relative_position++;
  }

  free(info);

  /*
   * Register domain for PDO exchange
   */
  master->domain_reg = malloc((all_pdo_count + 1) * sizeof(ec_pdo_entry_reg_t));
  ec_pdo_entry_reg_t *domain_reg_cur = master->domain_reg;

  for (size_t i = 0; i < master->slave_count; i++) {
    Ethercat_Slave_t *slave = master->slaves + i;
    slave->cyclic_mode = 0;  // mark slaves as not in cyclic mode
    for (int j = 0; j < slave->info->sync_count; j++) {
      ec_sync_info_t *sm = slave->sm_info + j;
      if (0 == sm->n_pdos) { /* if no PDOs for this sync manager proceed to the next one */
        continue;
      }

      pdo_t *values = NULL;

      if (sm->dir == EC_DIR_INPUT) {
        values = slave->input_values;
      } else {
        syslog(LOG_WARNING, "... skip wrong direction");
        continue; /* skip this configuration - FIXME better error handling? */
      }

      size_t valcount = 0;

      for (unsigned int m = 0; m < sm->n_pdos; m++) {
        ec_pdo_info_t *pdos = sm->pdos + m;

        for (unsigned int n = 0; n < pdos->n_entries; n++) {
          ec_pdo_entry_info_t *entry = pdos->entries + n;

          pdo_t *pdoe = (values + valcount);
          valcount++;

          /* FIXME Add proper error handling if VALUE_TYPE_NONE is returned */
          pdoe->type = ENTRY_TYPE_NONE;

          for (size_t i = 0; i < slave->sdo_count; i++) {
            Sdo_t *sdo = slave->dictionary + i;
            if (sdo->index == entry->index && sdo->subindex == entry->subindex) {
              pdoe->type = sdo->entry_type;
              break;
            }
          }

          if (pdoe->type == ENTRY_TYPE_NONE) {
            syslog(LOG_ERR, "Error, invalid PDO mapped: SDO (%#04x, %d) not "
                            "found", entry->index, entry->subindex);
          }

          // IMPORTANT: The reference alias must be used, as well as the
          // position relative to that alias
          domain_reg_cur->alias = slave->reference_alias;
          domain_reg_cur->position = slave->relative_position;

          domain_reg_cur->vendor_id = slave->info->vendor_id;
          domain_reg_cur->product_code = slave->info->product_code;
          domain_reg_cur->index = entry->index;
          domain_reg_cur->subindex = entry->subindex;
          domain_reg_cur->offset = &(pdoe->offset);
          domain_reg_cur->bit_position = &(pdoe->bit_offset);
          domain_reg_cur++;
        }
      }
    }
  }

  for (size_t i = 0; i < master->slave_count; i++) {
    Ethercat_Slave_t *slave = master->slaves + i;
    for (int j = 0; j < slave->info->sync_count; j++) {
      ec_sync_info_t *sm = slave->sm_info + j;
      if (0 == sm->n_pdos) { /* if no PDOs for this sync manager proceed to the next one */
        continue;
      }

      pdo_t *values = NULL;

      if (sm->dir == EC_DIR_OUTPUT) {
        values = slave->output_values;
      } else {
        syslog(LOG_WARNING, "... skip wrong direction");
        continue; /* skip this configuration - FIXME better error handling? */
      }

      size_t value_count = 0;

      for (unsigned int m = 0; m < sm->n_pdos; m++) {
        ec_pdo_info_t *pdos = sm->pdos + m;

        for (unsigned int n = 0; n < pdos->n_entries; n++) {
          ec_pdo_entry_info_t *entry = pdos->entries + n;

          pdo_t *pdoe = (values + value_count);
          value_count++;

          /* FIXME Add proper error handling if VALUE_TYPE_NONE is returned */
          pdoe->type = ENTRY_TYPE_NONE;

          for (size_t i = 0; i < slave->sdo_count; i++) {
            Sdo_t *sdo = slave->dictionary + i;
            if (sdo->index == entry->index && sdo->subindex == entry->subindex) {
              pdoe->type = sdo->entry_type;
              break;
            }
          }

          if (pdoe->type == ENTRY_TYPE_NONE) {
            syslog(LOG_ERR, "Error, invalid PDO mapped: SDO (%#04x, %d) not "
                            "found", entry->index, entry->subindex);
          }

          // IMPORTANT: The reference alias must be used, as well as the
          // position relative to that alias
          domain_reg_cur->alias = slave->reference_alias;
          domain_reg_cur->position = slave->relative_position;

          domain_reg_cur->vendor_id = slave->info->vendor_id;
          domain_reg_cur->product_code = slave->info->product_code;
          domain_reg_cur->index = entry->index;
          domain_reg_cur->subindex = entry->subindex;
          domain_reg_cur->offset = &(pdoe->offset);
          domain_reg_cur->bit_position = &(pdoe->bit_offset);
          domain_reg_cur++;
        }
      }
    }
  }
  // IMPORTANT: The last element in the domain registration must be a null
  // struct, or at least an ec_pdo_entry_reg_t with its index set to zero
  memset(domain_reg_cur, 0, sizeof(ec_pdo_entry_reg_t));

  update_master_state(master);
  update_all_slave_state(master);

  closelog();

  return master;
}

void ecw_master_release(Ethercat_Master_t *master)
{
  free_all_slaves(master); /* FIXME have to recursively clean up the slaves! */
  ecrt_release_master(master->master);
  free(master->domain_reg);
  free(master);
}

int ecw_master_start(Ethercat_Master_t *master)
{
  openlog(LIBETHERCAT_WRAPPER_SYSLOG, LOG_CONS | LOG_PID | LOG_NDELAY,
  LOG_USER);

  if (master->master == NULL) {
    syslog(LOG_ERR, "Error: Master not configured");
    return -1;
  }

  /* Slave configuration for the master */
  for (size_t slave_id = 0; slave_id < master->slave_count; slave_id++) {
    Ethercat_Slave_t *slave = master->slaves + slave_id;
    slave->cyclic_mode = 1;  // mark slaves as in cyclic mode

    slave->config = ecrt_master_slave_config(master->master, slave->reference_alias,
                                             slave->relative_position,
                                             slave->info->vendor_id,
                                             slave->info->product_code);

    if (slave->config == NULL) {
      syslog(LOG_ERR, "Error: Slave (id: %lu) configuration failed", slave_id);
      return -1;
    }

    if (setup_sdo_request(slave)) {
      syslog(LOG_ERR, "Error: Could not setup SDO requests for slave ID %lu",
               slave_id);
      return -1;
    }
  }

  /* create the domain registry */
  master->domain = ecrt_master_create_domain(master->master);
  if (master->domain == NULL) {
    syslog(LOG_ERR, "Error: Cannot create domain");
    return -1;
  }

  if (ecrt_domain_reg_pdo_entry_list(master->domain, master->domain_reg) != 0) {
    syslog(LOG_ERR, "Error: Cannot register PDO domain");
    return -1;
  }

  /* FIXME how can I get information about the error leading to no activation
   * of the master */
  if (ecrt_master_activate(master->master) < 0) {
    syslog(LOG_ERR, "Error: Could not activate master");
    return -1;
  }

  master->process_data = ecrt_domain_data(master->domain);
  if (master->process_data == NULL) {
    syslog(
        LOG_ERR,
        "Error: Unable to get the process data pointer. Disable master again.");
    ecrt_master_deactivate(master->master);
    return -1;
  }

  update_domain_state(master);

  closelog();

  return 0;
}

int ecw_master_stop(Ethercat_Master_t *master)
{
  /* FIXME Check if master is running */

  /* The documentation of this function in ecrt.h is kind of misleading. It
   * states that this function shouldn't be called in real-time context. On the
   * other hand, the official IgH documentation states this function as
   * counterpart to ecrt_master_activate(). */
  ecrt_master_deactivate(master->master);

  /* These pointer will become invalid after call to ecrt_master_deactivate() */
  master->domain = NULL;
  master->process_data = NULL;

  /* This function frees the following data structures (internally):
   *
   * Removes the bus configuration. All objects created by
   * ecrt_master_create_domain(), ecrt_master_slave_config(), ecrt_domain_data()
   * ecrt_slave_config_create_sdo_request() and
   * ecrt_slave_config_create_voe_handler() are freed, so pointers to them
   * become invalid.
   */

  /* mark slaves as not in cyclic mode */
  for (size_t slaveid = 0; slaveid < master->slave_count; slaveid++) {
    Ethercat_Slave_t *slave = master->slaves + slaveid;
    slave->cyclic_mode = 0;
  }

  return 0;
}

/**
 * @return number of found slaves; <0 on error
 */
int ecw_master_scan(Ethercat_Master_t *master)
{
  update_all_slave_state(master);
  return 0;
}

int ecw_master_rescan(Ethercat_Master_t *master)
{
  return ecrt_master_rescan(master->master);
}

int ecw_master_start_cyclic(Ethercat_Master_t *master)
{
  openlog(LIBETHERCAT_WRAPPER_SYSLOG, LOG_CONS | LOG_PID | LOG_NDELAY,
  LOG_USER);

  if (ecrt_master_activate(master->master)) {
    syslog(LOG_ERR, "[ERROR %s] Unable to activate the master", __func__);
    return -1;
  }

  if (!(master->process_data = ecrt_domain_data(master->domain))) {
    syslog(LOG_ERR, "[ERROR %s] Cannot access process data space", __func__);
    return -1;
  }

  closelog();

  return 0;
}

int ecw_master_stop_cyclic(Ethercat_Master_t *master)
{
  /* ecrt_master_deactivate() cleans up everything that was used for
   * the master application, during this process the pointers to the
   * generated structures become invalid. */
  master->process_data = NULL;
  master->domain = NULL;
  ecrt_master_deactivate(master->master);

  return 0;
}

/* TODO
 * this function provides the data exchange with the kernel module and has
 * to be called cyclically.
 */
int ecw_master_cyclic_function(Ethercat_Master_t *master)
{
  ecw_master_receive_pdo(master);

  update_domain_state(master);
  update_master_state(master);
  update_all_slave_state(master);

  ecw_master_send_pdo(master);

  return 0;
}

/*
 * Handler for cyclic tasks
 */

int ecw_master_pdo_exchange(Ethercat_Master_t *master)
{
  int ret = 0;

  /* receive has to be in front of send, FIXME may merge these two functions */
  ret = ecw_master_receive_pdo(master);
  ret = ecw_master_send_pdo(master);

  return ret;
}

int ecw_master_receive_pdo(Ethercat_Master_t *master)
{
  openlog(LIBETHERCAT_WRAPPER_SYSLOG, LOG_CONS | LOG_PID | LOG_NDELAY,
  LOG_USER);

  /* FIXME: Check error state and may add handler for broken topology. */
  ecrt_master_receive(master->master);
  ecrt_domain_process(master->domain);

  for (size_t i = 0; i < master->slave_count; i++) {
    Ethercat_Slave_t *slave = master->slaves + i;

    for (size_t k = 0; k < slave->in_pdo_count; k++) {
      pdo_t *pdo = ecw_slave_get_in_pdo(slave, k);

      switch (pdo->type) {
        case ENTRY_TYPE_BOOLEAN:
          pdo->value = EC_READ_BIT(master->process_data + pdo->offset,
                                   pdo->bit_offset);
          break;
        case ENTRY_TYPE_UNSIGNED8:
          pdo->value = EC_READ_U8(master->process_data + pdo->offset);
          break;
        case ENTRY_TYPE_UNSIGNED16:
          pdo->value = EC_READ_U16(master->process_data + pdo->offset);
          break;
        case ENTRY_TYPE_REAL32:
        case ENTRY_TYPE_UNSIGNED32:
          pdo->value = EC_READ_U32(master->process_data + pdo->offset);
          break;
        case ENTRY_TYPE_INTEGER8:
          pdo->value = EC_READ_S8(master->process_data + pdo->offset);
          break;
        case ENTRY_TYPE_INTEGER16:
          pdo->value = EC_READ_S16(master->process_data + pdo->offset);
          break;
        case ENTRY_TYPE_INTEGER32:
          pdo->value = EC_READ_S32(master->process_data + pdo->offset);
          break;
        default:
          //syslog(LOG_ERR, "Warning, unknown value type(%d). No RxPDO update", pdo->type);
          pdo->value = 0;
          break;
      }

      ecw_slave_set_in_pdo(slave, k, pdo);
    }
  }

  closelog();

  return 0;
}

int ecw_master_send_pdo(Ethercat_Master_t *master)
{
  openlog(LIBETHERCAT_WRAPPER_SYSLOG, LOG_CONS | LOG_PID | LOG_NDELAY,
  LOG_USER);

  for (size_t i = 0; i < master->slave_count; i++) {
    const Ethercat_Slave_t *slave = master->slaves + i;

    for (size_t k = 0; k < slave->out_pdo_count; k++) {
      pdo_t *value = ecw_slave_get_out_pdo(slave, k);

      // EC_WRITE_XX(master->process_data + (slave->txpdo_offset + k), value);
      switch (value->type) {
        case ENTRY_TYPE_BOOLEAN:
          EC_WRITE_BIT(master->process_data + value->offset, value->bit_offset,
                       value->value);
          break;
        case ENTRY_TYPE_UNSIGNED8:
          EC_WRITE_U8(master->process_data + value->offset, value->value);
          break;
        case ENTRY_TYPE_UNSIGNED16:
          EC_WRITE_U16(master->process_data + value->offset, value->value);
          break;
        case ENTRY_TYPE_REAL32:
        case ENTRY_TYPE_UNSIGNED32:
          EC_WRITE_U32(master->process_data + value->offset, value->value);
          break;
        case ENTRY_TYPE_INTEGER8:
          EC_WRITE_S8(master->process_data + value->offset, value->value);
          break;
        case ENTRY_TYPE_INTEGER16:
          EC_WRITE_S16(master->process_data + value->offset, value->value);
          break;
        case ENTRY_TYPE_INTEGER32:
          EC_WRITE_S32(master->process_data + value->offset, value->value);
          break;
        default:
          //syslog(LOG_ERR, "Warning, unknown value type(%d). No TxPDO update", value->type);
          break;
      }
    }
  }

  ecrt_domain_queue(master->domain);
  ecrt_master_send(master->master);

  closelog();

  return 0;
}

/*
 * Slave Handling
 */

size_t ecw_master_slave_count(Ethercat_Master_t *master)
{
  return master->slave_count;
}

size_t ecw_master_slave_responding(Ethercat_Master_t *master)
{
  update_master_state(master);
  return master->master_state.slaves_responding;
}

Ethercat_Slave_t *ecw_slave_get(Ethercat_Master_t *master, int slave_id)
{
  if (slave_id < 0 || (unsigned int) slave_id >= master->slave_count) {
    return NULL;
  }

  return (master->slaves + slave_id);
}

int ecw_slave_set_state(Ethercat_Master_t *master, int slave_id,
                        enum eALState state)
{
  uint8_t int_state = ALSTATE_INIT;
  switch (state) {
    case ALSTATE_INIT:
      int_state = 1;
      break;
    case ALSTATE_PREOP:
      int_state = 2;
      break;
    case ALSTATE_BOOT:
      int_state = 3;
      break;
    case ALSTATE_SAFEOP:
      int_state = 4;
      break;
    case ALSTATE_OP:
      int_state = 8;
      break;
  }

  return ecrt_master_slave_link_state_request(master->master, slave_id,
                                              int_state);
}

/*
 * State update functions
 */

static void update_domain_state(Ethercat_Master_t *master)
{
  ec_domain_state_t ds;
  ecrt_domain_state(master->domain, &ds/*&(master->domain_state)*/);

#if 0 /* for now disable this vast amount of prints FIXME have to figure out why this happend */
  if (ds.working_counter != master->domain_state.working_counter)
  syslog(LOG_ERR, "Working counter differ: %d / %d",
      ds.working_counter, master->domain_state.working_counter);

  if (ds.wc_state != master->domain_state.wc_state)
  syslog(LOG_ERR, "New WC State: %d / %d",
      ds.wc_state, master->domain_state.wc_state);
#endif

  master->domain_state = ds;
}

static void update_master_state(Ethercat_Master_t *master)
{
  ecrt_master_state(master->master, &(master->master_state));

  if (master->slave_count != master->master_state.slaves_responding) {
    syslog(LOG_ERR, "Warning slaves responding: %u expected: %zu",
           master->master_state.slaves_responding, master->slave_count);
  }
}

static void update_all_slave_state(Ethercat_Master_t *master)
{
  for (size_t i = 0; i < master->slave_count; i++) {
    Ethercat_Slave_t *slave = master->slaves + i;
    ecrt_slave_config_state(slave->config, &(slave->state));
  }
}

static void free_all_slaves(Ethercat_Master_t *master)
{
  for (size_t i = 0; i < master->slave_count; i++) {
    Ethercat_Slave_t *slave = master->slaves + i;
    free(slave->sm_info->pdos);
    free(slave->sm_info);
    free(slave->dictionary);
    free(slave->output_values);
    free(slave->input_values);
    free(slave->info);
    // The configurations (slave->config) and domains are being freed by the
    // ecrt_release_master() function
  }
  free(master->slaves);
}

void ecw_print_master_state(Ethercat_Master_t *master)
{
  ec_master_state_t state;
  ec_master_link_state_t link_state;
  printf("master state:\n");

  ecrt_master_state(master->master, &state);
  printf("slaves_responding: %d\nal_states: %d\nlink_up %d\n",
         state.slaves_responding, state.al_states, state.link_up);

  if (ecrt_master_link_state(master->master, 0, &link_state)) {
    printf("ecrt_master_link_state failed\n");
  } else {
    printf("slaves_responding: %d\nal_states: %d\nlink_up %d\n",
           link_state.slaves_responding, link_state.al_states,
           link_state.link_up);
  }
}

char * ecw_strerror(int error_num)
{
  switch (error_num) {
    case ECW_SUCCESS:
      return "ECW_SUCCESS";
      break;
    case ECW_ERROR_UNKNOWN:
      return "ECW_ERROR_UNKNOWN";
      break;
    case ECW_ERROR_SDO_REQUEST_BUSY:
      return "ECW_ERROR_SDO_REQUEST_BUSY";
      break;
    case ECW_ERROR_SDO_REQUEST_ERROR:
      return "ECW_ERROR_SDO_REQUEST_ERROR";
      break;
    case ECW_ERROR_SDO_NOT_FOUND:
      return "ECW_ERROR_SDO_NOT_FOUND";
      break;
    case ECW_ERROR_LINK_UP:
      return "ECW_ERROR_LINK_UP";
      break;
    case ECW_ERROR_SDO_UNSUPPORTED_BIT_LENGTH:
      return "ECW_ERROR_SDO_UNSUPPORTED_BIT_LENGTH";
      break;
  }
  return "ECW_ERROR_UNKNOWN";
}