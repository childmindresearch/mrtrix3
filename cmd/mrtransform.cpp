/*
    Copyright 2008 Brain Research Institute, Melbourne, Australia

    Written by J-Donald Tournier, 27/06/08.

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "app.h"
#include "progressbar.h"
#include "image/voxel.h"
#include "dataset/interp/nearest.h"
#include "dataset/interp/linear.h"
#include "dataset/interp/cubic.h"
#include "dataset/interp/reslice.h"
#include "dataset/misc.h"
#include "dataset/loop.h"
#include "dataset/copy.h"

using namespace MR; 

SET_VERSION_DEFAULT;
SET_AUTHOR (NULL);
SET_COPYRIGHT (NULL);

DESCRIPTION = {
 "apply spatial transformations or reslice images.",
 "In most cases, this command will only modify the transform matrix, "
   "without reslicing the image. Only the \"reslice\" option will "
   "actually modify the image data.",
  NULL
};

ARGUMENTS = {
  Argument ("input", "input image", "input image to be transformed.").type_image_in (),
  Argument ("output", "output image", "the output image.").type_image_out (),
  Argument::End
};


const char* interp_choices[] = { "nearest", "linear", "cubic", NULL };

OPTIONS = {

  Option ("transform", "the transform to use", 
      "specify the 4x4 transform to apply.")
    .append (Argument ("transform", "transform", 
          "the transform to apply, in the form of a 4x4 ascii file.").type_file ()),

  Option ("replace", "replace transform", 
      "replace the transform of the original image by that specified, "
      "rather than applying it to the original image."),

  Option ("inverse", "use inverse transform", 
      "invert the specified transform before using it."),

  Option ("reslice", "reslice to template image", 
      "reslice the input image to match the specified template image.")
    .append (Argument ("template", "template", "the template image.").type_image_in ()),

  Option ("reference", "reference image for transform", 
      "if the transform supplied maps from the input image onto a reference "
      "image (i.e. not to scanner coordinates), use this option to specify "
      "the reference image. Note that this implicitly sets the -replace option.")
    .append (Argument ("image", "image", "the reference image.").type_image_in ()),

  Option ("flipx", "assume x-flipped transform", 
      "assume the transform is supplied assuming a coordinate system with the "
      "x-axis reversed relative to the MRtrix convention (i.e. x increases "
      "from right to left). This is required to handle transform matrices "
      "produced by FSL's FLIRT command. This is only used in conjunction with "
      "the -reference option."),

  Option ("interp", "interpolation method", 
      "set the interpolation method to use when reslicing (default: linear).")
    .append (Argument ("method", "method", "the interpolation method.").type_choice (interp_choices)),

  Option ("oversample", "oversample", 
      "set the oversampling factor to use when reslicing (i.e. the "
      "number of samples to take per voxel along each spatial dimension). "
      "This should be supplied as a vector of 3 integers. By default, the "
      "oversampling factor is determined based on the differences between "
      "input and output voxel sizes."
    ).append (Argument ("factors", "factors", "the oversampling factors.").type_sequence_int()),

  Option ("datatype", "data type", "specify output image data type (default: same as input image).")
    .append (Argument ("spec", "specifier", "the data type specifier.").type_choice (DataType::identifiers)),

  Option::End 
};






EXECUTE {
  Math::Matrix<float> T;

  std::vector<OptBase> opt = get_options ("transform");
  if (opt.size()) {
    T.load (opt[0][0].get_string());
    if (T.rows() != 4 || T.columns() != 4) 
      throw Exception (std::string("transform matrix supplied in file \"") + opt[0][0].get_string() + "\" is not 4x4");
  }


  Image::Header header_in (argument[0].get_image());
  Image::Header header (header_in);

  opt = get_options ("datatype");
  if (opt.size()) header.datatype().parse (DataType::identifiers[opt[0][0].get_int()]);

  bool inverse = get_options("inverse").size();
  bool replace = get_options("replace").size();

  if (inverse) {
    if (!T.is_set()) 
      throw Exception ("no transform provided for option '-inverse' (specify using '-transform' option)");
    Math::Matrix<float> I;
    Math::LU::inv (I, T);
    T.swap (I);
  }

  opt = get_options("reference");
  if (opt.size()) {
    if (!T.is_set()) 
      throw Exception ("no transform provided for option '-reference' (specify using '-transform' option)");

    Image::Header ref_header (opt[0][0].get_image());

    if (get_options("flipx").size()) {
      Math::Matrix<float> R_ref(4,4);
      R_ref.identity();
      R_ref(0,0) = -1.0;

      Math::Matrix<float> R_orig (R_ref);

      R_ref(0,3) = (ref_header.dim(0)-1) * ref_header.vox(0);
      R_orig(0,3) = (header.dim(0)-1) * header.vox(0);

      if (inverse) 
        R_ref.swap (R_orig);

      Math::Matrix<float> T_tmp;
      Math::mult (T_tmp, T, R_orig);
      Math::mult (T, R_ref, T_tmp);
    }

    Math::Matrix<float> T_tmp;
    Math::mult (T_tmp, ref_header.transform(), T);
    T.swap (T_tmp);
    replace = true;
  }



  if (replace) 
    if (!T.is_set()) 
      throw Exception ("no transform provided for option '-replace' (specify using '-transform' option)");

  

  opt = get_options("reslice"); // need to reslice
  if (opt.size()) {
    Image::Header template_header (opt[0][0].get_image());
    header.axes[0].dim = template_header.axes[0].dim;
    header.axes[1].dim = template_header.axes[1].dim;
    header.axes[2].dim = template_header.axes[2].dim;
    header.axes[0].vox = template_header.axes[0].vox;
    header.axes[1].vox = template_header.axes[1].vox;
    header.axes[2].vox = template_header.axes[2].vox;
    header.transform() = template_header.transform();
    header.comments.push_back ("resliced to reference image \"" + template_header.name() + "\"");

    int interp = 1;
    opt = get_options ("interp");
    if (opt.size()) interp = opt[0][0].get_int();

    std::vector<int> oversample;
    opt = get_options ("oversample");
    if (opt.size()) {
      oversample = parse_ints (opt[0][0].get_string());
      if (oversample.size() != 3) throw Exception ("option \"oversample\" expects a vector of 3 values");
      if (oversample[0] < 1 || oversample[1] < 1 || oversample[2] < 1) 
        throw Exception ("oversample factors must be greater than zero");
    }

    if (replace) {
      header_in.transform().swap (T);
      T.clear();
    }

    const Image::Header header_out = argument[1].get_image (header);

    Image::Voxel<float> in (header_in);
    Image::Voxel<float> out (header_out);

    switch (interp) {
      case 0: DataSet::Interp::reslice<DataSet::Interp::Nearest> (out, in, T, oversample); break;
      case 1: DataSet::Interp::reslice<DataSet::Interp::Linear> (out, in, T, oversample); break;
      case 2: DataSet::Interp::reslice<DataSet::Interp::Cubic> (out, in, T, oversample); break;
      default: assert (0);
    }

  }
  else {
    // straight copy:
    if (T.is_set()) {
      header.comments.push_back ("transform modified");
      if (replace) 
        header.transform().swap (T);
      else {
        Math::Matrix<float> M (header.transform());
        Math::mult (header.transform(), T, M);
      }
    }

    const Image::Header header_out = argument[1].get_image (header);
    Image::Voxel<float> in (header_in);
    Image::Voxel<float> out (header_out);
    DataSet::copy_with_progress (out, in);
  }
}

