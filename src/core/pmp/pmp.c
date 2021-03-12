#include <stdio.h>
#include <string.h>

#include <pmp.h>
#include <riscv_helper.h>

// #define PMP_DEBUG_ENABLE
#ifdef PMP_DEBUG_ENABLE
#define PMP_DEBUG(...) do{ printf( __VA_ARGS__ ); } while( 0 )
#else
#define PMP_DEBUG(...) do{ } while ( 0 )
#endif

int pmp_write_csr_cfg(void *priv, privilege_level curr_priv, uint16_t reg_index, rv_uint_xlen csr_val)
{
    uint8_t i = 0;
    pmp_td *pmp = priv;
    uint8_t *cfg_ptr = (uint8_t *)&pmp->cfg[reg_index];
    uint8_t *new_val_ptr = (uint8_t *)&csr_val;

    /* All PMP cfgs can only be changed in machine mode */
    if(curr_priv != machine_mode)
        return RV_ACCESS_ERR;

    for(i=0;i<sizeof(rv_uint_xlen);i++)
    {
        /* check if it is locked, this can only be cleared by a reset */
        if(!CHECK_BIT(cfg_ptr[i], PMP_CFG_L_BIT))
        {
            cfg_ptr[i] = new_val_ptr[i];
        }
    }

    return RV_ACCESS_OK;
}

int pmp_read_csr_cfg(void *priv, privilege_level curr_priv_mode, uint16_t reg_index, rv_uint_xlen *out_val)
{
    (void) curr_priv_mode;

    pmp_td *pmp = priv;
    *out_val = pmp->cfg[reg_index];
    return RV_ACCESS_OK;
}

int pmp_write_csr_addr(void *priv, privilege_level curr_priv, uint16_t reg_index, rv_uint_xlen csr_val)
{
    pmp_td *pmp = priv;
    uint8_t *cfg_ptr = (uint8_t *)&pmp->cfg[reg_index/sizeof(rv_uint_xlen)];

    /* All PMP cfgs can only be changed in machine mode */
    if(curr_priv != machine_mode)
        return RV_ACCESS_ERR;

    /* updating the reg is only permitted if it is not locked 
       do nothing and return with OK if it is locked
    */
    if(CHECK_BIT(cfg_ptr[reg_index%sizeof(rv_uint_xlen)], PMP_CFG_L_BIT))
    {
        return RV_ACCESS_OK;
    }

    pmp->addr[reg_index] = csr_val;

    return RV_ACCESS_OK;
}

int pmp_read_csr_addr(void *priv, privilege_level curr_priv_mode, uint16_t reg_index, rv_uint_xlen *out_val)
{
    (void) curr_priv_mode;

    pmp_td *pmp = priv;
    *out_val = pmp->addr[reg_index];
    return RV_ACCESS_OK;
}

static inline rv_uint_xlen get_pmp_napot_size_from_pmpaddr(rv_uint_xlen addr)
{
    /* +2 because it was shifted by 2 with encoding */
    return (1 << (FIND_FIRST_BIT_SET(~addr) + 2) );
}

static inline rv_uint_xlen get_pmp_napot_addr_from_pmpaddr(rv_uint_xlen addr)
{
    rv_uint_xlen size = get_pmp_napot_size_from_pmpaddr(addr);
    rv_uint_xlen mask = ((size/2) - 1) >> 2;
    return (addr - mask) << 2;
}

int pmp_mem_check(pmp_td *pmp, privilege_level curr_priv, rv_uint_xlen addr, uint8_t len, pmp_access_type access_type)
{
    /* check if the address matches any enabled config */
    /* lower cfgs have precedence over higher ones */
    uint8_t i = 0;
    uint8_t j = 0;
    uint8_t addr_count = 0;
    uint8_t *cfg_ptr = NULL;
    pmp_addr_matching addr_mode = pmp_a_off;
    rv_uint_xlen addr_start = 0;
    rv_uint_xlen addr_size = 0;
    int at_least_one_active = 0;
    uint8_t curr_access_flags = (1 << access_type);
    uint8_t allowed_access = 0;

    /* We don't have to do any check if we are in machine mode */
    if(curr_priv == machine_mode)
        return RV_ACCESS_OK;

    for(i=0;i<PMP_NR_CFG_REGS;i++)
    {
        cfg_ptr = (uint8_t *)&pmp->cfg[i];
        for(j=0;j<sizeof(pmp->cfg[0]);j++)
        {
            addr_count = (i*sizeof(pmp->cfg[0])) + j;
            PMP_DEBUG("pmpaddr: "PRINTF_FMT"\n", pmp->addr[addr_count]);
            addr_mode = extract8(cfg_ptr[j], PMP_CFG_A_BIT_OFFS, 2);

            PMP_DEBUG("id: %d addr_mode: %x\n", j, addr_mode);

            if(!addr_mode)
                continue;

            allowed_access = cfg_ptr[j] & 0x7;

            at_least_one_active = 1;

            switch(addr_mode)
            {
                case pmp_a_tor:
                    if(addr_count==0)
                    {
                        addr_start = 0;
                        addr_size = (pmp->addr[addr_count] << 2);
                    }
                    else
                    {
                        addr_start = (pmp->addr[addr_count-1] << 2);
                        addr_size = (pmp->addr[addr_count] << 2) - addr_start;
                    }
                break;
                case pmp_a_napot:
                    /* I couldn't find this case in the spec, but qemu seems to do it in a similar fashion
                     * https://github.com/qemu/qemu/blob/master/target/riscv/pmp.c
                     */
                    if(pmp->addr[addr_count] == (rv_uint_xlen)-1)
                    {
                        addr_size = -1;
                        addr_start = 0;
                    }
                    else
                    {
                        addr_size = get_pmp_napot_size_from_pmpaddr(pmp->addr[addr_count]);
                        addr_start = get_pmp_napot_addr_from_pmpaddr(pmp->addr[addr_count]);
                    }
                break;
                case pmp_a_na4:
                    addr_start = (pmp->addr[addr_count] << 2);
                    addr_size = 4;
                break;
                default:
                break;
            }

            PMP_DEBUG("addr: " PRINTF_FMT "\n", addr);
            PMP_DEBUG("addr_start: " PRINTF_FMT "\n", addr_start);
            PMP_DEBUG("size: " PRINTF_FMT "\n", addr_size);

            if(ADDR_WITHIN_LEN(addr, len, addr_start, addr_size))
            {
                PMP_DEBUG("Addr match found!\n");
                if(!(curr_access_flags & allowed_access))
                {
                    PMP_DEBUG("Access type in match not allowed! curr_access_flags: %x allowed_access: %x\n", curr_access_flags, allowed_access);
                    return RV_ACCESS_ERR;
                }

                PMP_DEBUG("Address OK!\n");
                return RV_ACCESS_OK;
            }
        }
    }

    if(at_least_one_active)
    {
        PMP_DEBUG("No PMP match found!\n");
        return RV_ACCESS_ERR;
    }

    return RV_ACCESS_OK;
}

void pmp_dump_cfg_regs(pmp_td *pmp)
{
    uint8_t i = 0;
    uint8_t j = 0;
    uint8_t cnt = 0;
    uint8_t *cfg_ptr = NULL;
    (void) cfg_ptr;

    PMP_DEBUG("===== CFG REG DUMP =====\n");

    for(i=0;i<PMP_NR_CFG_REGS;i++)
    {
        PMP_DEBUG("REG %d:\n", i);
        cfg_ptr = (uint8_t *)&pmp->cfg[i];
        for(j=0;j<sizeof(pmp->cfg[0]);j++)
        {
            PMP_DEBUG("pmpcfg %d: %02x\n", cnt, cfg_ptr[j]);
            cnt++;
        }
        PMP_DEBUG("\n");
    }

    PMP_DEBUG("\n");
}
