/*
 * Broadcom STB SoCs Bus Unit Interface controls
 *
 * Copyright (C) 2015, Broadcom Corporation
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/syscore_ops.h>

#define  CPU_CREDIT_REG_MCPx_WR_PAIRING_EN_MASK 0x70000000
#define CPU_CREDIT_REG_MCPx_READ_CRED_MASK	0xf
#define CPU_CREDIT_REG_MCPx_WRITE_CRED_MASK	0xf
#define CPU_CREDIT_REG_MCPx_READ_CRED_SHIFT(x)	((x) * 8)
#define CPU_CREDIT_REG_MCPx_WRITE_CRED_SHIFT(x)	(((x) * 8) + 4)

#define CPU_MCP_FLOW_REG_MCPx_RDBUFF_CRED_SHIFT(x)	((x) * 8)
#define CPU_MCP_FLOW_REG_MCPx_RDBUFF_CRED_MASK		0xff

#define CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_THRESHOLD_MASK	0xf
#define CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_TIMEOUT_MASK		0xf
#define CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_TIMEOUT_SHIFT	4
#define CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_ENABLE		BIT(8)

static void __iomem *cpubiuctrl_base;
static unsigned int mcp_wr_pairing_en;
static const int *cpubiuctrl_regs;

static inline u32 cbc_readl(int reg)
{
	int offset = cpubiuctrl_regs[reg];

	if (offset == -1)
		return (u32)-1;

	return __raw_readl(cpubiuctrl_base + offset);
}

static inline void cbc_writel(u32 val, int reg)
{
	int offset = cpubiuctrl_regs[reg];

	if (offset == -1)
		return;

	__raw_writel(val,  cpubiuctrl_base + offset);
}

enum cpubiuctrl_regs {
	CPU_CREDIT_REG = 0,
	CPU_MCP_FLOW_REG,
	CPU_WRITEBACK_CTRL_REG
};

static const int b15_cpubiuctrl_regs[] = {
	[CPU_CREDIT_REG] = 0x184,
	[CPU_MCP_FLOW_REG] = -1,
	[CPU_WRITEBACK_CTRL_REG] = -1,
};

/* Odd cases, e.g: 7260/7268/7271A0 */
static const int b53_cpubiuctrl_no_wb_regs[] = {
	[CPU_CREDIT_REG] = 0x0b0,
	[CPU_MCP_FLOW_REG] = 0x0b4,
	[CPU_WRITEBACK_CTRL_REG] = -1,
};

static const int b53_cpubiuctrl_regs[] = {
	[CPU_CREDIT_REG] = 0x0b0,
	[CPU_MCP_FLOW_REG] = 0x0b4,
	[CPU_WRITEBACK_CTRL_REG] = 0x22c,
};

#define NUM_CPU_BIUCTRL_REGS	3

/*
 * HW7445-1920: On affected chips, disable MCP write pairing to improve
 * stability on long term stress test.
 */
static int __init mcp_write_pairing_set(void)
{
	u32 creds = 0;

	if (!cpubiuctrl_base)
		return -1;

	creds = cbc_readl(CPU_CREDIT_REG);
	if (mcp_wr_pairing_en) {
		pr_info("MCP: Enabling write pairing\n");
		cbc_writel(creds | CPU_CREDIT_REG_MCPx_WR_PAIRING_EN_MASK,
			   CPU_CREDIT_REG);
	} else if (creds & CPU_CREDIT_REG_MCPx_WR_PAIRING_EN_MASK) {
		pr_info("MCP: Disabling write pairing\n");
		cbc_writel(creds & ~CPU_CREDIT_REG_MCPx_WR_PAIRING_EN_MASK,
			   CPU_CREDIT_REG);
	} else {
		pr_info("MCP: Write pairing already disabled\n");
	}

	return 0;
}

static const char *b53_mach_compat[] = {
	"brcm,bcm7268b0",
	"brcm,bcm7271b0",
	"brcm,bcm7278a0",
};

static void __init mcp_b53_set(void)
{
	unsigned int i;
	u32 reg;

	for (i = 0; i < ARRAY_SIZE(b53_mach_compat); i++) {
		if (of_machine_is_compatible(b53_mach_compat[i]))
			break;
	}

	if (i == ARRAY_SIZE(b53_mach_compat))
		return;

	/* Set all 3 MCP interfaces to 8 credits */
	reg = cbc_readl(CPU_CREDIT_REG);
	for (i = 0; i < 3; i++) {
		reg &= ~(CPU_CREDIT_REG_MCPx_WRITE_CRED_MASK <<
			 CPU_CREDIT_REG_MCPx_WRITE_CRED_SHIFT(i));
		reg &= ~(CPU_CREDIT_REG_MCPx_READ_CRED_MASK <<
			 CPU_CREDIT_REG_MCPx_READ_CRED_SHIFT(i));
		reg |= 8 << CPU_CREDIT_REG_MCPx_WRITE_CRED_SHIFT(i);
		reg |= 8 << CPU_CREDIT_REG_MCPx_READ_CRED_SHIFT(i);
	}
	cbc_writel(reg, CPU_CREDIT_REG);

	/* Max out the number of in-flight Jwords reads on the MCP interface */
	reg = cbc_readl(CPU_MCP_FLOW_REG);
	for (i = 0; i < 3; i++)
		reg |= CPU_MCP_FLOW_REG_MCPx_RDBUFF_CRED_MASK <<
			CPU_MCP_FLOW_REG_MCPx_RDBUFF_CRED_SHIFT(i);
	cbc_writel(reg, CPU_MCP_FLOW_REG);

	/* Enable writeback throttling, set timeout to 128 cycles, 256 cycles
	 * threshold
	 */
	reg = cbc_readl(CPU_WRITEBACK_CTRL_REG);
	reg |= CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_ENABLE;
	reg &= ~CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_THRESHOLD_MASK;
	reg &= ~(CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_TIMEOUT_MASK <<
		 CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_TIMEOUT_SHIFT);
	reg |= 8;
	reg |= 7 << CPU_WRITEBACK_CTRL_REG_WB_THROTTLE_TIMEOUT_SHIFT;
	cbc_writel(reg, CPU_WRITEBACK_CTRL_REG);
}

static void __init setup_hifcpubiuctrl_regs(void)
{
	struct device_node *np, *cpu_dn;

	np = of_find_compatible_node(NULL, NULL, "brcm,brcmstb-cpu-biu-ctrl");
	if (!np)
		pr_err("missing BIU control node\n");

	cpubiuctrl_base = of_iomap(np, 0);
	if (!cpubiuctrl_base)
		pr_err("failed to remap BIU control base\n");

	mcp_wr_pairing_en = of_property_read_bool(np, "brcm,write-pairing");

	of_node_put(np);

	cpu_dn = of_get_cpu_node(0, NULL);
	if (!cpu_dn) {
		pr_err("failed to obtain CPU device node\n");
		return;
	}

	if (of_device_is_compatible(cpu_dn, "brcm,brahma-b15"))
		cpubiuctrl_regs = b15_cpubiuctrl_regs;
	else if (of_device_is_compatible(cpu_dn, "brcm,brahma-b53"))
		cpubiuctrl_regs = b53_cpubiuctrl_regs;
	else
		pr_err("unsupported CPU\n");
	of_node_put(cpu_dn);

	if (of_machine_is_compatible("brcm,bcm7260a0") ||
	    of_machine_is_compatible("brcm,bcm7268a0") ||
	    of_machine_is_compatible("brcm,bcm7271a0"))
		cpubiuctrl_regs = b53_cpubiuctrl_no_wb_regs;
}

#ifdef CONFIG_PM_SLEEP
/* for save/restore */
static u32 cpubiuctrl_reg_save[NUM_CPU_BIUCTRL_REGS];

static int brcmstb_cpu_credit_reg_suspend(void)
{
	unsigned int i;

	if (!cpubiuctrl_base)
		return 0;

	for (i = 0; i < NUM_CPU_BIUCTRL_REGS; i++)
		cpubiuctrl_reg_save[i] = cbc_readl(i);

	return 0;
}

static void brcmstb_cpu_credit_reg_resume(void)
{
	unsigned int i;

	if (!cpubiuctrl_base)
		return;

	for (i = 0; i < NUM_CPU_BIUCTRL_REGS; i++)
		cbc_writel(cpubiuctrl_reg_save[i], i);
}

static struct syscore_ops brcmstb_cpu_credit_syscore_ops = {
	.suspend = brcmstb_cpu_credit_reg_suspend,
	.resume = brcmstb_cpu_credit_reg_resume,
};
#endif


void __init brcmstb_biuctrl_init(void)
{
	setup_hifcpubiuctrl_regs();
	if (mcp_write_pairing_set())
		pr_err("MCP: Unable to disable write pairing!\n");

	mcp_b53_set();
#ifdef CONFIG_PM_SLEEP
	register_syscore_ops(&brcmstb_cpu_credit_syscore_ops);
#endif
}
