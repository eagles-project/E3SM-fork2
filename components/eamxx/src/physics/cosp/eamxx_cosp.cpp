#include "eamxx_cosp.hpp"
#include "cosp_functions.hpp"
#include "share/property_checks/field_within_interval_check.hpp"

#include "ekat/ekat_assert.hpp"
#include "ekat/util/ekat_units.hpp"

#include "share/field/field_utils.hpp"

#include <array>

namespace scream
{
// =========================================================================================
Cosp::Cosp (const ekat::Comm& comm, const ekat::ParameterList& params)
  : AtmosphereProcess(comm, params)
{
   // Determine how often to call COSP; units can be steps or hours
  m_cosp_frequency = m_params.get<Int>("cosp_frequency", 1);
  m_cosp_frequency_units = m_params.get<std::string>("cosp_frequency_units", "steps");
  EKAT_REQUIRE_MSG(
    (m_cosp_frequency_units == "steps") || (m_cosp_frequency_units == "hours"),
    "cosp_frequency_units " + m_cosp_frequency_units + " not supported"
  );

  // How many subcolumns to use for COSP
  m_num_subcols = m_params.get<Int>("cosp_subcolumns", 10);
}

// =========================================================================================
void Cosp::set_grids(const std::shared_ptr<const GridsManager> grids_manager)
{
  using namespace ekat::units;
  using namespace ekat::prefixes;
  using namespace ShortFieldTagsNames;

  // The units of mixing ratio Q are technically non-dimensional.
  // Nevertheless, for output reasons, we like to see 'kg/kg'.
  Units nondim = Units::nondimensional();
  Units percent (nondim,"%");
  auto micron = micro*m;
  auto m2 = pow(m, 2);
  auto s2 = pow(s, 2);
  // YQIN
  auto cm_tmp = m/100;
  auto cm3 = pow(cm_tmp,3);

  m_grid = grids_manager->get_grid("Physics");
  const auto& grid_name = m_grid->name();
  m_num_cols = m_grid->get_num_local_dofs(); // Number of columns on this rank
  m_num_levs = m_grid->get_num_vertical_levels();  // Number of levels per column

  // Define the different field layouts that will be used for this process

  // Layout for 3D (2d horiz X 1d vertical) variable defined at mid-level and interfaces
  FieldLayout scalar2d     = m_grid->get_2d_scalar_layout();
  FieldLayout scalar3d_mid = m_grid->get_3d_scalar_layout(true);
  FieldLayout scalar3d_int = m_grid->get_3d_scalar_layout(false);
  FieldLayout scalar4d_ctptau ( {COL,CMP,CMP},
                                {m_num_cols,m_num_tau,m_num_ctp},
                                {e2str(COL), "cosp_tau", "cosp_prs"});

  FieldLayout scalar4d_lwprel ( {COL,CMP,CMP},
                                {m_num_cols,m_num_lwp,m_num_rel},
                                {e2str(COL), "cosp_lwp", "cosp_reffliq"});
  FieldLayout scalar4d_iwprei ( {COL,CMP,CMP},
                                {m_num_cols,m_num_iwp,m_num_rei},
                                {e2str(COL), "cosp_iwp", "cosp_reffice"});


  FieldLayout scalar4d_cthtau ( {COL,CMP,CMP},
                                {m_num_cols,m_num_tau,m_num_cth},
                                {e2str(COL), "cosp_tau", "cosp_cth"});

  // Set of fields used strictly as input
  //                  Name in AD     Layout               Units   Grid       Group
  add_field<Required>("surf_radiative_T", scalar2d    , K,      grid_name);
  //add_field<Required>("surfelev",    scalar2d    , m,      grid_name);
  //add_field<Required>("landmask",    scalar2d    , nondim, grid_name);
  add_field<Required>("sunlit",           scalar2d    , nondim, grid_name);
  add_field<Required>("p_mid",             scalar3d_mid, Pa,     grid_name);
  add_field<Required>("p_int",             scalar3d_int, Pa,     grid_name);
  //add_field<Required>("height_mid",  scalar3d_mid, m,      grid_name);
  //add_field<Required>("height_int",  scalar3d_int, m,      grid_name);
  add_field<Required>("T_mid",            scalar3d_mid, K,      grid_name);
  add_field<Required>("phis",             scalar2d    , m2/s2,  grid_name);
  add_field<Required>("pseudo_density",   scalar3d_mid, Pa,     grid_name);
  add_field<Required>("cldfrac_rad",      scalar3d_mid, nondim, grid_name);
  add_tracer<Required>("qv", m_grid, kg/kg);
  add_tracer<Required>("qc", m_grid, kg/kg);
  add_tracer<Required>("qi", m_grid, kg/kg);
  // Optical properties, should be computed in radiation interface
  add_field<Required>("dtau067",     scalar3d_mid, nondim, grid_name); // 0.67 micron optical depth
  add_field<Required>("dtau105",     scalar3d_mid, nondim, grid_name); // 10.5 micron optical depth
  // Effective radii, should be computed in either microphysics or radiation interface
  // TODO: should these be meters or microns? Was meters before, but using "m" instead
  // of "micron" seemed to cause prim_model_finalize to throw error with the following:
  // ABORTING WITH ERROR: Error! prim_init_model_f90 was not called yet (or prim_finalize_f90 was already called).
  // P3 defines this field with micron instead of meters units, so is this a unit conversion issue?
  add_field<Required>("eff_radius_qc",     scalar3d_mid, micron,      grid_name);
  add_field<Required>("eff_radius_qi",     scalar3d_mid, micron,      grid_name);
  // Set of fields used strictly as output
  add_field<Computed>("isccp_cldtot", scalar2d, percent, grid_name);
  add_field<Computed>("isccp_ctptau", scalar4d_ctptau, percent, grid_name, 1);
  add_field<Computed>("modis_ctptau", scalar4d_ctptau, percent, grid_name, 1);

  // MODIS
  add_field<Computed>("modis_cldtot", scalar2d, percent, grid_name);
  add_field<Computed>("modis_clwtot", scalar2d, percent, grid_name);
  add_field<Computed>("modis_clitot", scalar2d, percent, grid_name);
  add_field<Computed>("modis_taut", scalar2d, nondim, grid_name);
  add_field<Computed>("modis_tauw", scalar2d, nondim, grid_name);
  add_field<Computed>("modis_taui", scalar2d, nondim, grid_name);
  add_field<Computed>("modis_reffw", scalar2d, m, grid_name);
  add_field<Computed>("modis_reffi", scalar2d, m, grid_name);
  add_field<Computed>("modis_lwp", scalar2d, kg/m2, grid_name);
  add_field<Computed>("modis_iwp", scalar2d, kg/m2, grid_name);

  add_field<Computed>("modis_cld_Q06", scalar2d, nondim, grid_name);
  add_field<Computed>("modis_nd_Q06", scalar2d, 1/cm3, grid_name);
  add_field<Computed>("modis_lwp_Q06", scalar2d, kg/m2, grid_name);
  add_field<Computed>("modis_tau_Q06", scalar2d, nondim, grid_name);
  add_field<Computed>("modis_reff_Q06", scalar2d, m, grid_name);

  add_field<Computed>("modis_cld_ALL", scalar2d, nondim, grid_name);
  add_field<Computed>("modis_nd_ALL", scalar2d, 1/cm3, grid_name);
  add_field<Computed>("modis_lwp_ALL", scalar2d, kg/m2, grid_name);
  add_field<Computed>("modis_tau_ALL", scalar2d, nondim, grid_name);
  add_field<Computed>("modis_reff_ALL", scalar2d, m, grid_name);

  add_field<Computed>("modis_ctptau_liq", scalar4d_ctptau, percent, grid_name, 1);
  add_field<Computed>("modis_ctptau_ice", scalar4d_ctptau, percent, grid_name, 1);
  add_field<Computed>("modis_lwpre", scalar4d_lwprel, percent, grid_name, 1);
  add_field<Computed>("modis_iwpre", scalar4d_iwprei, percent, grid_name, 1);

  add_field<Computed>("misr_cthtau", scalar4d_cthtau, percent, grid_name, 1);
  add_field<Computed>("cosp_sunlit", scalar2d, nondim, grid_name);
}

// =========================================================================================
void Cosp::initialize_impl (const RunType /* run_type */)
{
  // Set property checks for fields in this process
  CospFunc::initialize(m_num_cols, m_num_subcols, m_num_levs);


  // Add note to output files about processing ISCCP fields that are only valid during
  // daytime. This can go away once I/O can handle masked time averages.
  using stratts_t = std::map<std::string,std::string>;
  std::list<std::string> vnames = {"isccp_cldtot", "isccp_ctptau", "modis_ctptau", "misr_cthtau", "modis_ctptau_liq", "modis_ctptau_ice", "modis_lwpre", "modis_iwpre",
                                   "modis_cldtot", "modis_clwtot", "modis_clitot", "modis_taut", "modis_tauw", "modis_taui", "modis_reffw", "modis_reffi", "modis_lwp", "modis_iwp",
                                   "modis_cld_Q06", "modis_nd_Q06", "modis_lwp_Q06", "modis_tau_Q06", "modis_reff_Q06",
                                   "modis_cld_ALL", "modis_nd_ALL", "modis_lwp_ALL", "modis_tau_ALL", "modis_reff_ALL"};
  for (const auto field_name : {"isccp_cldtot", "isccp_ctptau", "modis_ctptau", "misr_cthtau", "modis_ctptau_liq", "modis_ctptau_ice", "modis_lwpre", "modis_iwpre",
                                "modis_cldtot", "modis_clwtot", "modis_clitot", "modis_taut", "modis_tauw", "modis_taui", "modis_reffw", "modis_reffi", "modis_lwp", "modis_iwp",
                                   "modis_cld_Q06", "modis_nd_Q06", "modis_lwp_Q06", "modis_tau_Q06", "modis_reff_Q06",
                                   "modis_cld_ALL", "modis_nd_ALL", "modis_lwp_ALL", "modis_tau_ALL", "modis_reff_ALL"}) {
      auto& f = get_field_out(field_name);
      auto& atts = f.get_header().get_extra_data<stratts_t>("io: string attributes");
      atts["note"] = "Night values are zero; divide by cosp_sunlit to get daytime mean";
  }
}

// =========================================================================================
void Cosp::run_impl (const double dt)
{
  // Determine if we should update COSP this timestep
  // First get frequency in steps
  int cosp_freq_in_steps = 1;
  if (m_cosp_frequency_units == "steps") {
      cosp_freq_in_steps = m_cosp_frequency;
  } else if (m_cosp_frequency_units == "hours") {
      EKAT_REQUIRE_MSG((3600 % int(dt)) == 0, "cosp_frequency_units is hours but dt does not evenly divide 1 hour");
      cosp_freq_in_steps = 3600.0 * m_cosp_frequency / dt;
  } else {
      EKAT_ERROR_MSG("cosp_frequency_units " + m_cosp_frequency_units + " not supported");
  }

  // Make sure cosp frequency is multiple of rad frequency?

  // Compare frequency in steps with current timestep
  auto ts = timestamp();
  auto update_cosp = cosp_do(cosp_freq_in_steps, ts.get_num_steps());

  // Get fields from field manager; note that we get host views because this
  // interface serves primarily as a wrapper to a c++ to f90 bridge for the COSP
  // all then need to be copied to layoutLeft views to permute the indices for
  // F90.
  //
  // Need to make sure device data is synced to host?
  get_field_in("qv").sync_to_host();
  get_field_in("qc").sync_to_host();
  get_field_in("qi").sync_to_host();
  get_field_in("sunlit").sync_to_host();
  get_field_in("surf_radiative_T").sync_to_host();
  get_field_in("T_mid").sync_to_host();
  get_field_in("p_mid").sync_to_host();
  get_field_in("p_int").sync_to_host();
  get_field_in("cldfrac_rad").sync_to_host();
  get_field_in("eff_radius_qc").sync_to_host();
  get_field_in("eff_radius_qi").sync_to_host();
  get_field_in("dtau067").sync_to_host();
  get_field_in("dtau105").sync_to_host();
  get_field_in("phis").sync_to_host();
  get_field_in("pseudo_density").sync_to_host();

  auto qv      = get_field_in("qv").get_view<const Real**, Host>();
  auto qc      = get_field_in("qc").get_view<const Real**, Host>();
  auto qi      = get_field_in("qi").get_view<const Real**, Host>();
  auto sunlit  = get_field_in("sunlit").get_view<const Real*, Host>();
  auto skt     = get_field_in("surf_radiative_T").get_view<const Real*, Host>();
  auto T_mid   = get_field_in("T_mid").get_view<const Real**, Host>();
  auto p_mid   = get_field_in("p_mid").get_view<const Real**, Host>();
  auto p_int   = get_field_in("p_int").get_view<const Real**, Host>();
  auto phis    = get_field_in("phis").get_view<const Real*, Host>();
  auto pseudo_density = get_field_in("pseudo_density").get_view<const Real**, Host>();
  auto cldfrac = get_field_in("cldfrac_rad").get_view<const Real**, Host>();
  auto reff_qc = get_field_in("eff_radius_qc").get_view<const Real**, Host>();
  auto reff_qi = get_field_in("eff_radius_qi").get_view<const Real**, Host>();
  auto dtau067 = get_field_in("dtau067").get_view<const Real**, Host>();
  auto dtau105 = get_field_in("dtau105").get_view<const Real**, Host>();
  auto isccp_cldtot = get_field_out("isccp_cldtot").get_view<Real*, Host>();
  auto isccp_ctptau = get_field_out("isccp_ctptau").get_view<Real***, Host>();
  auto modis_ctptau = get_field_out("modis_ctptau").get_view<Real***, Host>();

  auto modis_cldtot = get_field_out("modis_cldtot").get_view<Real*, Host>();
  auto modis_clwtot = get_field_out("modis_clwtot").get_view<Real*, Host>();
  auto modis_clitot = get_field_out("modis_clitot").get_view<Real*, Host>();
  auto modis_taut = get_field_out("modis_taut").get_view<Real*, Host>();
  auto modis_tauw = get_field_out("modis_tauw").get_view<Real*, Host>();
  auto modis_taui = get_field_out("modis_taui").get_view<Real*, Host>();
  auto modis_reffw = get_field_out("modis_reffw").get_view<Real*, Host>();
  auto modis_reffi = get_field_out("modis_reffi").get_view<Real*, Host>();
  auto modis_lwp = get_field_out("modis_lwp").get_view<Real*, Host>();
  auto modis_iwp = get_field_out("modis_iwp").get_view<Real*, Host>();

  auto modis_cld_Q06 = get_field_out("modis_cld_Q06").get_view<Real*, Host>();
  auto modis_nd_Q06 = get_field_out("modis_nd_Q06").get_view<Real*, Host>();
  auto modis_lwp_Q06 = get_field_out("modis_lwp_Q06").get_view<Real*, Host>();
  auto modis_tau_Q06 = get_field_out("modis_tau_Q06").get_view<Real*, Host>();
  auto modis_reff_Q06 = get_field_out("modis_reff_Q06").get_view<Real*, Host>();

  auto modis_cld_ALL = get_field_out("modis_cld_ALL").get_view<Real*, Host>();
  auto modis_nd_ALL = get_field_out("modis_nd_ALL").get_view<Real*, Host>();
  auto modis_lwp_ALL = get_field_out("modis_lwp_ALL").get_view<Real*, Host>();
  auto modis_tau_ALL = get_field_out("modis_tau_ALL").get_view<Real*, Host>();
  auto modis_reff_ALL = get_field_out("modis_reff_ALL").get_view<Real*, Host>();


  auto modis_ctptau_liq = get_field_out("modis_ctptau_liq").get_view<Real***, Host>();
  auto modis_ctptau_ice = get_field_out("modis_ctptau_ice").get_view<Real***, Host>();
  auto modis_lwpre = get_field_out("modis_lwpre").get_view<Real***, Host>();
  auto modis_iwpre = get_field_out("modis_iwpre").get_view<Real***, Host>();

  auto misr_cthtau  = get_field_out("misr_cthtau").get_view<Real***, Host>();
  auto cosp_sunlit  = get_field_out("cosp_sunlit").get_view<Real*, Host>();  // Copy of sunlit flag with COSP frequency for proper averaging

  // Compute heights
  const auto z_mid = CospFunc::view_2d<Real>("z_mid", m_num_cols, m_num_levs);
  const auto z_int = CospFunc::view_2d<Real>("z_int", m_num_cols, m_num_levs+1);
  const auto dz = z_mid;  // reuse tmp memory for dz
  const auto ncol = m_num_cols;
  const auto nlev = m_num_levs;
  // calculate_z_int contains a team-level parallel_scan, which requires a special policy
  // TODO: do this on device?
  const auto scan_policy = ekat::ExeSpaceUtils<KTH::ExeSpace>::get_thread_range_parallel_scan_team_policy(ncol, nlev);
  Kokkos::parallel_for(scan_policy, KOKKOS_LAMBDA (const KTH::MemberType& team) {
      const int i = team.league_rank();
      const auto dz_s    = ekat::subview(dz,    i);
      const auto p_mid_s = ekat::subview(p_mid, i);
      const auto T_mid_s = ekat::subview(T_mid, i);
      const auto qv_s = ekat::subview(qv, i);
      const auto z_int_s = ekat::subview(z_int, i);
      const auto z_mid_s = ekat::subview(z_mid, i);
      const Real z_surf  = phis(i) / 9.81;
      const auto pseudo_density_s = ekat::subview(pseudo_density, i);
      PF::calculate_dz(team, pseudo_density_s, p_mid_s, T_mid_s, qv_s, dz_s);
      team.team_barrier();
      PF::calculate_z_int(team,nlev,dz_s,z_surf,z_int_s);
      team.team_barrier();
      PF::calculate_z_mid(team,nlev,z_int_s,z_mid_s);
      team.team_barrier();
  });

  // Call COSP wrapper routines
  if (update_cosp) {
    Real emsfc_lw = 0.99;
    Kokkos::deep_copy(cosp_sunlit, sunlit);
    CospFunc::view_2d<const Real> z_mid_c = z_mid;  // Need a const version of z_mid for call to CospFunc::main
    CospFunc::main(
            m_num_cols, m_num_subcols, m_num_levs, m_num_tau, m_num_ctp, m_num_cth,
            m_num_lwp, m_num_iwp, m_num_rel, m_num_rei, 
            emsfc_lw, sunlit, skt, T_mid, p_mid, p_int, z_mid_c, qv, qc, qi,
            cldfrac, reff_qc, reff_qi, dtau067, dtau105,
            isccp_cldtot, isccp_ctptau, modis_ctptau, misr_cthtau,
            modis_ctptau_liq, modis_ctptau_ice, modis_lwpre, modis_iwpre,
            modis_cldtot, modis_clwtot, modis_clitot,
            modis_taut, modis_tauw, modis_taui,
            modis_reffw, modis_reffi, modis_lwp, modis_iwp,
            modis_cld_Q06, modis_nd_Q06, modis_lwp_Q06, modis_tau_Q06, modis_reff_Q06,
            modis_cld_ALL, modis_nd_ALL, modis_lwp_ALL, modis_tau_ALL, modis_reff_ALL
    );
    // Remask night values to ZERO since our I/O does not know how to handle masked/missing values
    // in temporal averages; this is all host data, so we can just use host loops like its the 1980s
    for (int i = 0; i < m_num_cols; i++) {
        if (sunlit(i) == 0) {
            isccp_cldtot(i) = 0;
            modis_cldtot(i) = 0;
            modis_clwtot(i) = 0; 
            modis_clitot(i) = 0; 
            modis_taut(i) = 0;
            modis_tauw(i) = 0;
            modis_taui(i) = 0; 
            modis_reffw(i) = 0;
            modis_reffi(i) = 0; 
            modis_lwp(i) = 0; 
            modis_iwp(i) = 0; 

            modis_cld_Q06(i) = 0;
            modis_nd_Q06(i) = 0;
            modis_lwp_Q06(i) = 0;
            modis_tau_Q06(i) = 0;
            modis_reff_Q06(i) = 0;

            modis_cld_ALL(i) = 0;
            modis_nd_ALL(i) = 0;
            modis_lwp_ALL(i) = 0;
            modis_tau_ALL(i) = 0;
            modis_reff_ALL(i) = 0;

            for (int j = 0; j < m_num_tau; j++) {
                for (int k = 0; k < m_num_ctp; k++) {
                    isccp_ctptau(i,j,k) = 0;
                    modis_ctptau(i,j,k) = 0;
                    modis_ctptau_liq(i,j,k) = 0;
                    modis_ctptau_ice(i,j,k) = 0; 
                }
                for (int k = 0; k < m_num_cth; k++) {
                    misr_cthtau (i,j,k) = 0;
                }
            }
            for (int j = 0; j < m_num_lwp; j++) {
                for (int k = 0; k < m_num_rel; k++) {
                    modis_lwpre(i,j,k) = 0;
                }
            }
            for (int j = 0; j < m_num_iwp; j++) {
                for (int k = 0; k < m_num_rei; k++) {
                    modis_iwpre(i,j,k) = 0;
                }
            }

        }
    }
  } else {
    // If not updating COSP statistics, set these to ZERO; this essentially weights
    // the ISCCP cloud properties by the sunlit mask. What will be output for time-averages
    // then is the time-average mask-weighted statistics; to get true averages, we need to
    // divide by the time-average of the mask. I.e., if M is the sunlit mask, and X is the ISCCP
    // statistic, then
    //
    //     avg(X) = sum(M * X) / sum(M) = (sum(M * X)/N) / (sum(M)/N) = avg(M * X) / avg(M)
    //
    // TODO: mask this when/if the AD ever supports masked averages
    Kokkos::deep_copy(isccp_cldtot, 0.0);
    Kokkos::deep_copy(isccp_ctptau, 0.0);
    Kokkos::deep_copy(modis_ctptau, 0.0);
    Kokkos::deep_copy(misr_cthtau, 0.0);
    Kokkos::deep_copy(cosp_sunlit, 0.0);

    Kokkos::deep_copy(modis_cldtot, 0.0);
    Kokkos::deep_copy(modis_clwtot, 0.0);
    Kokkos::deep_copy(modis_clitot, 0.0);
    Kokkos::deep_copy(modis_taut, 0.0);
    Kokkos::deep_copy(modis_tauw, 0.0);
    Kokkos::deep_copy(modis_taui, 0.0);
    Kokkos::deep_copy(modis_reffw, 0.0);
    Kokkos::deep_copy(modis_reffi, 0.0);
    Kokkos::deep_copy(modis_lwp, 0.0);
    Kokkos::deep_copy(modis_iwp, 0.0);

    Kokkos::deep_copy(modis_cld_Q06, 0.0);
    Kokkos::deep_copy(modis_nd_Q06, 0.0);
    Kokkos::deep_copy(modis_lwp_Q06, 0.0);
    Kokkos::deep_copy(modis_tau_Q06, 0.0);
    Kokkos::deep_copy(modis_reff_Q06, 0.0);

    Kokkos::deep_copy(modis_cld_ALL, 0.0);
    Kokkos::deep_copy(modis_nd_ALL, 0.0);
    Kokkos::deep_copy(modis_lwp_ALL, 0.0);
    Kokkos::deep_copy(modis_tau_ALL, 0.0);
    Kokkos::deep_copy(modis_reff_ALL, 0.0);

    Kokkos::deep_copy(modis_ctptau_liq, 0.0);
    Kokkos::deep_copy(modis_ctptau_ice, 0.0);
    Kokkos::deep_copy(modis_lwpre, 0.0);
    Kokkos::deep_copy(modis_iwpre, 0.0);

  }
  get_field_out("isccp_cldtot").sync_to_dev();
  get_field_out("isccp_ctptau").sync_to_dev();
  get_field_out("modis_ctptau").sync_to_dev();
  get_field_out("misr_cthtau").sync_to_dev();
  get_field_out("cosp_sunlit").sync_to_dev();

  get_field_out("modis_cldtot").sync_to_dev();
  get_field_out("modis_clwtot").sync_to_dev();
  get_field_out("modis_clitot").sync_to_dev();
  get_field_out("modis_taut").sync_to_dev();
  get_field_out("modis_tauw").sync_to_dev();
  get_field_out("modis_taui").sync_to_dev();
  get_field_out("modis_reffw").sync_to_dev();
  get_field_out("modis_reffi").sync_to_dev();
  get_field_out("modis_lwp").sync_to_dev();
  get_field_out("modis_iwp").sync_to_dev();

  get_field_out("modis_cld_Q06").sync_to_dev();
  get_field_out("modis_nd_Q06").sync_to_dev();
  get_field_out("modis_lwp_Q06").sync_to_dev();
  get_field_out("modis_tau_Q06").sync_to_dev();
  get_field_out("modis_reff_Q06").sync_to_dev();

  get_field_out("modis_cld_ALL").sync_to_dev();
  get_field_out("modis_nd_ALL").sync_to_dev();
  get_field_out("modis_lwp_ALL").sync_to_dev();
  get_field_out("modis_tau_ALL").sync_to_dev();
  get_field_out("modis_reff_ALL").sync_to_dev();

  get_field_out("modis_ctptau_liq").sync_to_dev();
  get_field_out("modis_ctptau_ice").sync_to_dev();
  get_field_out("modis_lwpre").sync_to_dev();
  get_field_out("modis_iwpre").sync_to_dev();

}

// =========================================================================================
void Cosp::finalize_impl()
{
  // Finalize COSP wrappers
  CospFunc::finalize();
}
// =========================================================================================

} // namespace scream
