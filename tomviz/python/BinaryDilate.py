import tomviz.operators


class BinaryDilate(tomviz.operators.CancelableOperator):

    def transform_scalars(self, dataset, structuring_element_id=0, radius=1,
                          object_label=1, background_label=0):
        """Dilate segmented objects with a given label by a spherically symmetric
        structuring element with a given radius.
        """

        # Initial progress
        self.progress.value = 0
        self.progress.maximum = 100

        # Approximate percentage of work completed after each step in the
        # transform
        STEP_PCT = [10, 20, 90, 100]

        try:
            import itk
            from tomviz import itkutils
        except Exception as exc:
            print("Could not import necessary module(s)")
            raise exc

        # Add a try/except around the ITK portion. ITK exceptions are
        # passed up to the Python layer, so we can at least report what
        # went wrong with the script, e.g,, unsupported image type.
        try:
            self.progress.value = STEP_PCT[0]
            self.progress.message = "Converting data to ITK image"

            # Get the ITK image
            itk_image = itkutils.convert_vtk_to_itk_image(dataset)
            itk_input_image_type = type(itk_image)

            itk_kernel_type = itk.FlatStructuringElement[3]
            if (structuring_element_id == 0):
                itk_kernel = itk_kernel_type.Box(radius)
            elif (structuring_element_id == 1):
                itk_kernel = itk_kernel_type.Ball(radius)
            elif (structuring_element_id == 2):
                itk_kernel = itk_kernel_type.Cross(radius)
            else:
                raise Exception('Invalid kernel shape id %d' %
                                structuring_element_id)

            self.progress.value = STEP_PCT[1]
            self.progress.message = "Running filter"

            dilate_filter = itk.BinaryDilateImageFilter[itk_input_image_type,
                                                        itk_input_image_type,
                                                        itk_kernel_type].New()
            dilate_filter.SetDilateValue(object_label)
            dilate_filter.SetBackgroundValue(background_label)
            dilate_filter.SetKernel(itk_kernel)
            dilate_filter.SetInput(itk_image)
            itkutils.observe_filter_progress(self, dilate_filter,
                                             STEP_PCT[1], STEP_PCT[2])

            try:
                dilate_filter.Update()
            except RuntimeError:
                return

            self.progress.message = "Saving results"

            itkutils.set_array_from_itk_image(dataset,
                                              dilate_filter.GetOutput())

            self.progress.value = STEP_PCT[3]
        except Exception as exc:
            print("Problem encountered while running %s" %
                  self.__class__.__name__)
            raise exc
