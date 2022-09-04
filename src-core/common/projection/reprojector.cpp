#include "reprojector.h"
#include "logger.h"
#include "warp/warp.h"
#include "gcp_compute/gcp_compute.h"

#include "sat_proj/sat_proj.h"

#include "projs/equirectangular.h"
#include "projs/stereo.h"
#include "projs/tpers.h"

namespace satdump
{
    namespace reprojection
    {
        ProjectionResult reproject(ReprojectionOperation &op, float *progress)
        {
            ProjectionResult result_prj;

            result_prj.img.init(op.output_width, op.output_height, 3);

            auto &image = op.img;
            auto &projected_image = result_prj.img;

            if (op.use_draw_algorithm)
            // Use old algorithm, less prone to errors on bad data and faster.
            // Mostly a copy-paste of the old implementation
            {
                logger->info("Using old algorithm...");

                // Target projs
                geodetic::projection::EquirectangularProjection equi_proj;
                geodetic::projection::StereoProjection stereo_proj;
                geodetic::projection::TPERSProjection tpers_proj;

                // Source projs
                geodetic::projection::EquirectangularProjection equi_proj_src;
                std::shared_ptr<SatelliteProjection> sat_proj_src;

                // Init
                std::function<std::pair<int, int>(float, float, int, int)> projectionFunction;

                if (op.target_prj_info["type"] == "equirectangular")
                {
                    equi_proj.init(projected_image.width(), projected_image.height(),
                                   op.target_prj_info["tl_lon"].get<float>(), op.target_prj_info["tl_lat"].get<float>(),
                                   op.target_prj_info["br_lon"].get<float>(), op.target_prj_info["br_lat"].get<float>());

                    projectionFunction = [&equi_proj](float lat, float lon, int map_height, int map_width) -> std::pair<int, int>
                    {
                        int imageLat = 0;
                        int imageLon = 0;
                        equi_proj.forward(lon, lat, imageLon, imageLat);
                        return {imageLon, imageLat};
                    };
                }
                else if (op.target_prj_info["type"] == "stereo")
                {
                    stereo_proj.init(op.target_prj_info["center_lat"].get<float>(), op.target_prj_info["center_lon"].get<float>());
                    float stereo_scale = op.target_prj_info["scale"].get<float>();

                    projectionFunction = [&stereo_proj, stereo_scale](float lat, float lon, int map_height, int map_width) -> std::pair<int, int>
                    {
                        double x = 0;
                        double y = 0;
                        stereo_proj.forward(lon, lat, x, y);
                        x *= map_width / stereo_scale;
                        y *= map_height / stereo_scale;
                        return {x + (map_width / 2), map_height - (y + (map_height / 2))};
                    };
                }
                else if (op.target_prj_info["type"] == "tpers")
                {
                    tpers_proj.init(op.target_prj_info["alt"].get<float>() * 1000,
                                    op.target_prj_info["lon"].get<float>(),
                                    op.target_prj_info["lat"].get<float>(),
                                    op.target_prj_info["ang"].get<float>(),
                                    op.target_prj_info["azi"].get<float>());

                    projectionFunction = [&tpers_proj](float lat, float lon, int map_height, int map_width) -> std::pair<int, int>
                    {
                        double x, y;
                        tpers_proj.forward(lon, lat, x, y);
                        x *= map_width / 2;
                        y *= map_height / 2;
                        return {x + (map_width / 2), map_height - (y + (map_height / 2))};
                    };
                }

                // Reproj
                if (op.source_prj_info["type"] == "equirectangular")
                {
                }
                else if (op.source_prj_info["type"] == "geos")
                {
                }
                else // ""TPS""" (Mostly LEO)
                {
                    sat_proj_src = get_sat_proj(op.source_prj_info, op.img_tle, op.img_tim);

                    for (int currentScan = 0; currentScan < (int)image.height(); currentScan++)
                    {
                        // Now compute each pixel's lat / lon and plot it
                        for (double px = 0; px < image.width() - 1; px += 1)
                        {
                            geodetic::geodetic_coords_t coords1, coords2;
                            bool ret1 = sat_proj_src->get_position(px, currentScan, coords1);
                            bool ret2 = sat_proj_src->get_position(px + 1, currentScan, coords2);

                            if (ret1 || ret2)
                                continue;

                            std::pair<float, float> map_cc1 = projectionFunction(coords1.lat, coords1.lon, projected_image.height(), projected_image.width());
                            std::pair<float, float> map_cc2 = projectionFunction(coords2.lat, coords2.lon, projected_image.height(), projected_image.width());

                            uint16_t color[3] = {0, 0, 0};
                            if (image.channels() >= 3)
                            {
                                color[0] = image[image.width() * image.height() * 0 + currentScan * image.width() + int(px)];
                                color[1] = image[image.width() * image.height() * 1 + currentScan * image.width() + int(px)];
                                color[2] = image[image.width() * image.height() * 2 + currentScan * image.width() + int(px)];
                            }
                            else
                            {
                                color[0] = image[currentScan * image.width() + int(px)];
                                color[1] = image[currentScan * image.width() + int(px)];
                                color[2] = image[currentScan * image.width() + int(px)];
                            }

                            if (color[0] == 0 && color[1] == 0 && color[2] == 0) // Skip Black
                                continue;

                            // This seems to glitch out sometimes... Need to check
                            double maxSize = projected_image.width() / 100.0;
                            if (abs(map_cc1.first - map_cc2.first) < maxSize && abs(map_cc1.second - map_cc2.second) < maxSize)
                            {
                                /*
                                Using a circle and radius from the next sample is most likely not the best way, computing the instrument's
                                actual FOV at each pixel and so would be a better approach... But well, this one has the benefit of being
                                fast.
                                I guess time will tell how reliable that approximation is.
                                */
                                double circle_radius = sqrt(pow(int(map_cc1.first - map_cc2.first), 2) + pow(int(map_cc1.second - map_cc2.second), 2));
                                projected_image.draw_circle(map_cc1.first, map_cc1.second /*+ circle_radius * 2.0*/, ceil(circle_radius), color, true); //, 0.4 * opacity);
                            }

                            // projected_image.draw_point(map_cc1.first, map_cc1.second, color, opacity);
                        }

                        if (progress != nullptr)
                            *progress = float(currentScan) / float(image.height());
                        logger->critical("{:d}/{:d}", currentScan, image.height());
                    }
                }
            }
            else
            {
                logger->info("Using new algorithm...");

                // Here, we first project to an equirectangular target
                image::Image<uint16_t> warped_image;
                float tl_lon, tl_lat;
                float br_lon, br_lat;

                // Reproject to equirect
                if (op.source_prj_info["type"] == "equirectangular")
                {
                    warped_image = op.img;
                    tl_lon = op.source_prj_info["tl_lon"].get<float>();
                    tl_lat = op.source_prj_info["tl_lat"].get<float>();
                    br_lon = op.source_prj_info["br_lon"].get<float>();
                    br_lat = op.source_prj_info["br_lat"].get<float>();
                }
                // else if (op.source_prj_info["type"] == "geos")
                //{
                // }
                else // Means it's a TPS-handled warp.
                {
                    warp::WarpOperation operation;
                    operation.ground_control_points = satdump::gcp_compute::compute_gcps(op.source_prj_info, op.img_tle, op.img_tim);
                    operation.input_image = op.img;
                    operation.output_width = 2048 * 10; // TODO : CHANGE!!!!!!
                    operation.output_height = 1024 * 10;

                    satdump::warp::ImageWarper warper;
                    warper.op = operation;
                    warper.update();

                    satdump::warp::WarpResult result = warper.warp();

                    warped_image = result.output_image;
                    tl_lon = result.top_left.lon;
                    tl_lat = result.top_left.lat;
                    br_lon = result.bottom_right.lon;
                    br_lat = result.bottom_right.lat;
                }

                logger->info("Reprojecting to target...");

                // Reproject to target
                if (op.target_prj_info["type"] == "equirectangular")
                {
                    geodetic::projection::EquirectangularProjection equi_proj;
                    equi_proj.init(projected_image.width(), projected_image.height(),
                                   op.target_prj_info["tl_lon"].get<float>(), op.target_prj_info["tl_lat"].get<float>(),
                                   op.target_prj_info["br_lon"].get<float>(), op.target_prj_info["br_lat"].get<float>());
                    geodetic::projection::EquirectangularProjection equi_proj_src;
                    equi_proj_src.init(warped_image.width(), warped_image.height(), tl_lon, tl_lat, br_lon, br_lat);

                    float lon, lat;
                    int x2, y2;
                    for (int x = 0; x < projected_image.width(); x++)
                    {
                        for (int y = 0; y < projected_image.height(); y++)
                        {
                            equi_proj.reverse(x, y, lon, lat);
                            if (lon == -1 || lat == -1)
                                continue;
                            equi_proj_src.forward(lon, lat, x2, y2);
                            if (x2 == -1 || y2 == -1)
                                continue;

                            if (warped_image.channels() == 3)
                                for (int c = 0; c < 3; c++)
                                    projected_image.channel(c)[y * projected_image.width() + x] = warped_image.channel(c)[y2 * warped_image.width() + x2];
                            else
                                for (int c = 0; c < 3; c++)
                                    projected_image.channel(c)[y * projected_image.width() + x] = warped_image.channel(0)[y2 * warped_image.width() + x2];
                        }
                    }
                }
                else if (op.target_prj_info["type"] == "stereo")
                {
                    geodetic::projection::StereoProjection stereo_proj;
                    stereo_proj.init(op.target_prj_info["center_lat"].get<float>(), op.target_prj_info["center_lon"].get<float>());
                    float stereo_scale = op.target_prj_info["scale"].get<float>();
                    geodetic::projection::EquirectangularProjection equi_proj_src;
                    equi_proj_src.init(warped_image.width(), warped_image.height(), tl_lon, tl_lat, br_lon, br_lat);

                    double lon, lat;
                    int x2, y2;
                    for (int x = 0; x < projected_image.width(); x++)
                    {
                        for (int y = 0; y < projected_image.height(); y++)
                        {
                            double px = (x - double(projected_image.width() / 2));
                            double py = (projected_image.height() - double(y)) - double(projected_image.height() / 2);

                            px /= projected_image.width() / stereo_scale;
                            py /= projected_image.height() / stereo_scale;

                            stereo_proj.inverse(px, py, lon, lat);
                            if (lon == -1 || lat == -1)
                                continue;

                            equi_proj_src.forward(lon, lat, x2, y2);
                            if (x2 == -1 || y2 == -1)
                                continue;

                            if (warped_image.channels() == 3)
                                for (int c = 0; c < 3; c++)
                                    projected_image.channel(c)[y * projected_image.width() + x] = warped_image.channel(c)[y2 * warped_image.width() + x2];
                            else
                                for (int c = 0; c < 3; c++)
                                    projected_image.channel(c)[y * projected_image.width() + x] = warped_image.channel(0)[y2 * warped_image.width() + x2];
                        }
                    }
                }
                else if (op.target_prj_info["type"] == "tpers")
                {
                    geodetic::projection::TPERSProjection tpers_proj;
                    tpers_proj.init(op.target_prj_info["alt"].get<float>() * 1000,
                                    op.target_prj_info["lon"].get<float>(),
                                    op.target_prj_info["lat"].get<float>(),
                                    op.target_prj_info["ang"].get<float>(),
                                    op.target_prj_info["azi"].get<float>());
                    geodetic::projection::EquirectangularProjection equi_proj_src;
                    equi_proj_src.init(warped_image.width(), warped_image.height(), tl_lon, tl_lat, br_lon, br_lat);

                    double lon, lat;
                    int x2, y2;
                    for (int x = 0; x < projected_image.width(); x++)
                    {
                        for (int y = 0; y < projected_image.height(); y++)
                        {
                            double px = (x - double(projected_image.width() / 2));
                            double py = (projected_image.height() - double(y)) - double(projected_image.height() / 2);

                            px /= projected_image.width() / 2;
                            py /= projected_image.height() / 2;

                            tpers_proj.inverse(px, py, lon, lat);
                            if (lon == -1 || lat == -1)
                                continue;

                            equi_proj_src.forward(lon, lat, x2, y2);
                            if (x2 == -1 || y2 == -1)
                                continue;

                            if (warped_image.channels() == 3)
                                for (int c = 0; c < 3; c++)
                                    projected_image.channel(c)[y * projected_image.width() + x] = warped_image.channel(c)[y2 * warped_image.width() + x2];
                            else
                                for (int c = 0; c < 3; c++)
                                    projected_image.channel(c)[y * projected_image.width() + x] = warped_image.channel(0)[y2 * warped_image.width() + x2];
                        }
                    }
                }
            }

            return result_prj;
        }
    }
}